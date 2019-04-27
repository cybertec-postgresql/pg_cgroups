#ifndef __linux__
#error "Linux control groups are only available on Linux"
#endif

#include "postgres.h"
#include "fmgr.h"

#include "miscadmin.h"
#include "storage/ipc.h"
#include "utils/guc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "pg_cgroups.h"

PG_MODULE_MAGIC;

static char *pg_cgroups_version;

/* GUCs defined by the module */
static int memory_limit = -1;
static int swap_limit = -1;
static bool oom_killer = true;
static char *read_bps_limit = NULL;
static char *write_bps_limit = NULL;
static char *read_iops_limit = NULL;
static char *write_iops_limit = NULL;
static int cpu_share = -1;
static char* cpus = NULL;	/* set during module initialization */
static char* memory_nodes = NULL;	/* set during module initialization */

/* other static variables */
static int max_cpu_share = -1;	/* set during module initialization */

/* static functions declarations */
static bool memory_limit_check(int *newval, void **extra, GucSource source);
static void memory_limit_assign(int newval, void *extra);
static void swap_limit_assign(int newval, void *extra);
static void oom_killer_assign(bool newval, void *extra);
static bool device_limit_check(char **newval, void **extra, GucSource source);
static void device_limit_assign(char * const limit_name, char *newval);
static void read_bps_limit_assign(const char *newval, void *extra);
static void write_bps_limit_assign(const char *newval, void *extra);
static void read_iops_limit_assign(const char *newval, void *extra);
static void write_iops_limit_assign(const char *newval, void *extra);
static bool cpu_share_check(int *newval, void **extra, GucSource source);
static void cpu_share_assign(int newval, void *extra);
static bool parse_online(char * const online, int *pmin, int *pmax);
static bool cpuset_check(char * const newval, char * const online);
static bool cpus_check(char **newval, void **extra, GucSource source);
static void cpus_assign(const char *newval, void *extra);
static bool memory_nodes_check(char **newval, void **extra, GucSource source);
static void memory_nodes_assign(const char *newval, void *extra);

void
_PG_init(void)
{
	int dummy, num_cpus;

	if (!process_shared_preload_libraries_in_progress)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("\"pg_cgroups\" must be added to \"shared_preload_libraries\"")));

	/* initialize cgroups library and set get GUC defaults */
	cg_init();

	/* set a default value (and upper limit) for cpu_share */
	if (!parse_online(get_def_cpus(), &dummy, &num_cpus))
		elog(FATAL, "internal error getting CPU count");

	max_cpu_share = (num_cpus + 1) * 100000;

	/* once the control group is set up, we can define the GUCs */
	DefineCustomIntVariable(
		"pg_cgroups.memory_limit",
		"Limit the RAM available to this cluster.",
		"This corresponds to \"memory.limit_in_bytes\".",
		&memory_limit,
		-1,
		-1,
		INT_MAX / 2,
		PGC_SIGHUP,
		GUC_UNIT_MB,
		memory_limit_check,
		memory_limit_assign,
		NULL
	);

	DefineCustomIntVariable(
		"pg_cgroups.swap_limit",
		"Limit the swap space available to this cluster.",
		"This corresponds to \"memory.memsw.limit_in_bytes\" minus \"memory.limit_in_bytes\".",
		&swap_limit,
		-1,
		-1,
		INT_MAX / 2,
		PGC_SIGHUP,
		GUC_UNIT_MB,
		NULL,
		swap_limit_assign,
		NULL
	);

	DefineCustomBoolVariable(
		"pg_cgroups.oom_killer",
		"Determines how to treat processes that exceed the memory limit.",
		"This corresponds to the negation of \"memory.oom_control\".",
		&oom_killer,
		true,
		PGC_SIGHUP,
		0,
		NULL,
		oom_killer_assign,
		NULL
	);

	DefineCustomStringVariable(
		"pg_cgroups.read_bps_limit",
		"Sets the read I/O limit per device in bytes.",
		"This corresponds to \"blkio.throttle.read_bps_device\".",
		&read_bps_limit,
		"",
		PGC_SIGHUP,
		0,
		device_limit_check,
		read_bps_limit_assign,
		NULL
	);

	DefineCustomStringVariable(
		"pg_cgroups.write_bps_limit",
		"Sets the write I/O limit per device in bytes.",
		"This corresponds to \"blkio.throttle.write_bps_device\".",
		&write_bps_limit,
		"",
		PGC_SIGHUP,
		0,
		device_limit_check,
		write_bps_limit_assign,
		NULL
	);

	DefineCustomStringVariable(
		"pg_cgroups.read_iops_limit",
		"Sets the read I/O limit per device in I/O operations per second.",
		"This corresponds to \"blkio.throttle.read_iops_device\".",
		&read_iops_limit,
		"",
		PGC_SIGHUP,
		0,
		device_limit_check,
		read_iops_limit_assign,
		NULL
	);

	DefineCustomStringVariable(
		"pg_cgroups.write_iops_limit",
		"Sets the write I/O limit per device in I/O operations per second.",
		"This corresponds to \"blkio.throttle.write_iops_device\".",
		&write_iops_limit,
		"",
		PGC_SIGHUP,
		0,
		device_limit_check,
		write_iops_limit_assign,
		NULL
	);

	DefineCustomIntVariable(
		"pg_cgroups.cpu_share",
		"Limit share of the available CPU time (100000 = 1 core).",
		"This corresponds to \"cpu.cfs_quota_us\".",
		&cpu_share,
		-1,
		-1,
		max_cpu_share,
		PGC_SIGHUP,
		0,
		cpu_share_check,
		cpu_share_assign,
		NULL
	);

	DefineCustomStringVariable(
		"pg_cgroups.cpus",
		"Specifies which CPUs are available for this cluster.",
		"This corresponds to \"cpuset.cpus\".",
		&cpus,
		strdup(get_def_cpus()),
		PGC_SIGHUP,
		0,
		cpus_check,
		cpus_assign,
		NULL
	);

	DefineCustomStringVariable(
		"pg_cgroups.memory_nodes",
		"Specifies which memory nodes are available for this cluster.",
		"This corresponds to \"cpuset.mems\".",
		&memory_nodes,
		strdup(get_def_memory_nodes()),
		PGC_SIGHUP,
		0,
		memory_nodes_check,
		memory_nodes_assign,
		NULL
	);

	DefineCustomStringVariable(
		"pg_cgroups.version",
		"The version of pg_cgroups.",
		NULL,
		&pg_cgroups_version,
		PG_CGROUPS_VERSION,
		PGC_INTERNAL,
		0,
		NULL,
		NULL,
		NULL
	);

	EmitWarningsOnPlaceholders("pg_cgroups");
}

bool
memory_limit_check(int *newval, void **extra, GucSource source)
{
	return (bool) (*newval != 0);
}

void
memory_limit_assign(int newval, void *extra)
{
	int64_t mem_value, swap_value, newtotal;

	/* only the postmaster changes the kernel */
	if (MyProcPid != PostmasterPid)
		return;

	/* convert from MB to bytes */
	mem_value = (newval == -1) ? -1 : newval * (int64_t)1048576;

	/* calculate the new value for swap_limit */
	if (newval == -1 || swap_limit == -1)
		newtotal = -1;
	else
		newtotal = (int64_t) swap_limit + newval;

	/* convert from MB to bytes */
	swap_value = (newtotal == -1) ? -1 : newtotal * 1048576;

	if (newval == -1
		|| (newval > memory_limit && memory_limit != -1))
	{
		/* we have to raise the limit on memory + swap first */
		cg_set_int64(CONTROLLER_MEMORY, "memory.memsw.limit_in_bytes", swap_value);
		cg_set_int64(CONTROLLER_MEMORY, "memory.limit_in_bytes", mem_value);
	}
	else
	{
		/* we have to lower the limit on memory + swap last */
		cg_set_int64(CONTROLLER_MEMORY, "memory.limit_in_bytes", mem_value);
		cg_set_int64(CONTROLLER_MEMORY, "memory.memsw.limit_in_bytes", swap_value);
	}
}

void
swap_limit_assign(int newval, void *extra)
{
	int64_t swap_value, newtotal;

	/* only the postmaster changes the kernel */
	if (MyProcPid != PostmasterPid)
		return;

	/* calculate the new memory + swap */
	if (memory_limit == -1 || newval == -1)
	{
		newtotal = -1;
		newval = -1;
	}
	else
		newtotal = (int64_t) newval + memory_limit;

	/* convert from MB to bytes */
	swap_value = (newtotal == -1) ? -1 : newtotal * 1048576;

	cg_set_int64(CONTROLLER_MEMORY, "memory.memsw.limit_in_bytes", swap_value);
}

void
oom_killer_assign(bool newval, void *extra)
{
	int64_t oom_value = !newval;

	/* only the postmaster changes the kernel */
	if (MyProcPid != PostmasterPid)
		return;

	cg_set_int64(CONTROLLER_MEMORY, "memory.oom_control", oom_value);
}

bool
device_limit_check(char **newval, void **extra, GucSource source)
{
	char *val = pstrdup(*newval),
		 *freeme = val;

	if (*val == '\0')
		return true;

	/* loop through comma-separated list */
	while (val)
	{
		char *nextp, *device, *limit, *filename;
		bool have_colon = false,
			 have_digit = false;
		struct stat statbuf;

		if ((nextp = strchr(val, ',')) != NULL)
		{
			*nextp = '\0';
			++nextp;
		}

		/* parse entry of the form <major>:<minor> <limit> */
		device = val;
		while (*val != ' ')
		{
			if (*val >= '0' && *val <= '9')
				have_digit = true;
			else if (*val == ':')
			{
				if (have_colon || !have_digit)
				{
					GUC_check_errdetail(
						"Entry \"%s\" does not start with \"major:minor\" device numbers.",
						device
					);
					return false;
				}

				have_colon = true;
				have_digit = false;
			}
			else if (*val == '\0')
			{
				GUC_check_errdetail(
					"Entry \"%s\" must have a space between device and limit.",
					device
				);
				return false;
			}
			else
			{
				GUC_check_errdetail(
					"Entry \"%s\" does not start with \"major:minor\" device numbers.",
					device
				);
				return false;
			}

			++val;
		}
		if (!have_colon || !have_digit)
		{
			GUC_check_errdetail(
				"Entry \"%s\" does not start with \"major:minor\" device numbers.",
				device
			);
			return false;
		}

		*(val++) = '\0';
		while (*val == ' ')
			++val;
		limit = val;

		have_digit = false;
		while (*val >= '0' && *val <= '9')
		{
			have_digit = true;
			++val;
		}
		if (*val != '\0' || !have_digit)
		{
			GUC_check_errdetail(
				"Limit \"%s\" must be an integer number.",
				limit
			);
			return false;
		}

		/* check if the device exists */
		filename = palloc(strlen(device) + 12);
		strcpy(filename, "/dev/block/");
		strcat(filename, device);

		errno = 0;
		if (stat(filename, &statbuf))
		{
			GUC_check_errdetail(
				errno == ENOENT ? "Device file \"%s\" does not exist."
								: "Error accessing device file \"%s\": %m",
				filename
			);
			return false;
		}

		if ((statbuf.st_mode & S_IFMT) != S_IFBLK)
		{
			GUC_check_errdetail(
				"Device file \"%s\" is not a block device.",
				filename
			);
			return false;
		}

		pfree(filename);

		val = nextp;
	}

	pfree(freeme);
	return true;
}

void
device_limit_assign(char * const limit_name, char *newval)
{
	int i;
	char *device_limit_val = NULL;

	/* only the postmaster changes the kernel */
	if (MyProcPid != PostmasterPid)
		return;

	device_limit_val = pstrdup(newval ? newval : "");

	/* replace commas with line breaks */
	if (device_limit_val)
		for (i=strlen(device_limit_val)-1; i>=0; --i)
			if (device_limit_val[i] == ',')
				device_limit_val[i] = '\n';

	cg_set_string(CONTROLLER_BLKIO, limit_name, device_limit_val);

	pfree(device_limit_val);
}

void
read_bps_limit_assign(const char *newval, void *extra)
{
	device_limit_assign("blkio.throttle.read_bps_device", (char *) newval);
}

void
write_bps_limit_assign(const char *newval, void *extra)
{
	device_limit_assign("blkio.throttle.write_bps_device", (char *) newval);
}

void
read_iops_limit_assign(const char *newval, void *extra)
{
	device_limit_assign("blkio.throttle.read_iops_device", (char *) newval);
}

void
write_iops_limit_assign(const char *newval, void *extra)
{
	device_limit_assign("blkio.throttle.write_iops_device", (char *) newval);
}

bool
cpu_share_check(int *newval, void **extra, GucSource source)
{
	return (bool) (*newval == -1 || *newval >= 1000);
}

void
cpu_share_assign(int newval, void *extra)
{
	/* only the postmaster changes the kernel */
	if (MyProcPid != PostmasterPid)
		return;

	cg_set_int64(CONTROLLER_CPU, "cpu.cfs_quota_us", (int64_t) newval);
}

/*
 * Extracts the first and the last number from a string that starts
 * and ends with a number.
 */
bool
parse_online(char * const online, int *pmin, int *pmax)
{
	char *start, *p, buf[100];

	/* we read from the start to get the first number */
	start = p = online;
	while (*p >= '0' && *p <= '9')
		++p;
	if (start == p || p - start >= 6)
	{
		GUC_check_errdetail(
			"Online limit \"%s\" does not start with a valid number.",
			online
		);

		return false;
	}
	memcpy(buf, start, p - start);
	buf[p - start] = '\0';
	*pmin = atoi(buf);

	/* now we read backwards from the end for the second number */
	p = start += strlen(online);
	while (start > online && *(start-1) >= '0' && *(start-1) <= '9')
		--start;
	if (start == p || p - start >= 6)
	{
		GUC_check_errdetail(
			"Online limit \"%s\" does not end with a valid number.",
			online
		);

		return false;
	}
	*pmax = atoi(start);

	return true;
}

/*
 * "newval" is parsed and checked if it matches the regexp
 * ^[0-9]+\(-[0-9]+\)?\(,[0-9]+\(-[0-9]+\)?\)*
 * "online" is of the form "m-n" and specifies the limits
 * for the numbers that appera in "newval".
 */
bool
cpuset_check(char * const newval, char * const online)
{
	int online_min, online_max, min = 0, max = 0;
	char *start, *p, buf[100];
	/*
	 * values for "state":
	 * 0: before comma group
	 * 1: in first number
	 * 2: after hyphen
	 * 3: in second number
	 */
	int state = 0;

	/* we take the first and the last number in "online" as limits */
	if (!parse_online(online, &online_min, &online_max))
		return false;

	/* parse and check newval */
	for (p = newval; true; ++p)
	{
		if (*p >= '0' && *p <= '9')
		{
			if (state == 0 || state == 2)
			{
				start = p;
				++state;
			}
		}
		else if (*p == '-')
		{
			if (state != 1)
			{
				GUC_check_errdetail(
					"Value \"%s\" has \"-\" in an invalid place.",
					newval
				);

				return false;
			}

			if (p - start >= 6)
			{
				GUC_check_errdetail(
					"Value \"%s\" contains an invalid number.",
					newval
				);

				return false;
			}

			memcpy(buf, start, p - start);
			buf[p - start] = '\0';
			min = atoi(buf);

			if (min < online_min || min > online_max)
			{
				GUC_check_errdetail(
					"Number %d is outside of range %d-%d.",
					min, online_min, online_max
				);

				return false;
			}

			state = 2;
		}
		else if (*p == ',' || *p == '\0')
		{
			if (state != 1 && state != 3)
			{
				GUC_check_errdetail(
					"Value \"%s\" is missing a number at the end of a group.",
					newval
				);

				return false;
			}

			if (p - start >= 6)
			{
				GUC_check_errdetail(
					"Value \"%s\" contains an invalid number.",
					newval
				);

				return false;
			}

			memcpy(buf, start, p - start);
			buf[p - start] = '\0';
			max = atoi(buf);

			if (state == 1 && (max < online_min || max > online_max))
			{
				GUC_check_errdetail(
					"Number %d is outside of range %d-%d.",
					max, online_min, online_max
				);

				return false;
			}

			if (state == 3 && (max < min || max > online_max))
			{
				GUC_check_errdetail(
					"Number %d is outside of range %d-%d.",
					max, min, online_max
				);

				return false;
			}

			if (*p == '\0')
				break;
			else
				state = 0;
		}
		else
		{
			GUC_check_errdetail(
				"Value \"%s\" contains an invalid character.",
				newval
			);

			return false;
		}
	}

	return true;
}

bool
cpus_check(char **newval, void **extra, GucSource source)
{
	return cpuset_check(*newval, get_def_cpus());
}

void
cpus_assign(const char *newval, void *extra)
{
	/* only the postmaster changes the kernel */
	if (MyProcPid != PostmasterPid)
		return;

	cg_set_string(CONTROLLER_CPUSET, "cpuset.cpus", (char *) newval);
}

bool
memory_nodes_check(char **newval, void **extra, GucSource source)
{
	return cpuset_check(*newval, get_def_memory_nodes());
}

void
memory_nodes_assign(const char *newval, void *extra)
{
	/* only the postmaster changes the kernel */
	if (MyProcPid != PostmasterPid)
		return;

	cg_set_string(CONTROLLER_CPUSET, "cpuset.mems", (char *) newval);
}
