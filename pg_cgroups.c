#ifndef __linux__
#error "Linux control groups are only available on Linux"
#endif

#include "postgres.h"
#include "fmgr.h"

#include <fcntl.h>
#include <errno.h>
#include <libcgroup.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "miscadmin.h"
#include "storage/ipc.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

/* global variables */
static char cg_name[100];

/*
 * These GUCs don't hold the authoritative values, those are in the
 * kernel and have to be fetched every time.
 */
static int memory_limit = -1;
static int swap_limit = -1;

/* exported function declarations */
extern void _PG_init(void);

/* static functions declarations */
static char * const get_online(char * const what);
static int64_t cg_get_int64(char * const controller, char * const property);
static void cg_set_int64(char * const controller, char * const property, int64_t value);
static void get_current_memory_limits(void);
static bool memory_limit_check(int *newval, void **extra, GucSource source);
static void memory_limit_assign(int newval, void *extra);
static const char *memory_limit_show(void);
static void swap_limit_assign(int newval, void *extra);
static const char *swap_limit_show(void);
static void on_exit_callback(int code, Datum arg);

void
_PG_init(void)
{
	int rc;
	pid_t pid;
	struct cgroup *cg;
	struct cgroup_controller *cg_memory;
	struct cgroup_controller *cg_cpu;
	struct cgroup_controller *cg_blkio;
	struct cgroup_controller *cg_cpuset;

	if (!process_shared_preload_libraries_in_progress)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("\"pg_cgroups\" must be added to \"shared_preload_libraries\"")));

	/* initialize cgroups library */
	if ((rc = cgroup_init()))
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot initialize cgroups library: %s", cgroup_strerror(rc))));

	/*
	 * Before we create our new cgroup, we have to set some required properties on the
	 * (hopefully) existing "/postgres" cgroup.  We could require the user to add them
	 * to /etc/cgconfig.conf, but it seems more robust and user-friendly to set them
	 * ourselves.
	 * Moreover, this is a good test if the "/postgres" cgroup has been set up correctly.
	 */
	if (!(cg = cgroup_new_cgroup("/postgres")))
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot create struct cgroup \"/postgres\"")));

	if ((rc = cgroup_get_cgroup(cg)))
	{
		cgroup_free(&cg);
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot read cgroup \"/postgres\" from kernel"),
				 errdetail("system error: %s", cgroup_strerror(rc)),
				 errhint("Configure the \"/postgres\" cgroup properly before starting the server.")));
	}

	/* set "cpuset.cpus" and "cpuset.cpus" to good defaults */
	if ((rc = cgroup_set_value_string(
					cgroup_get_controller(cg, "cpuset"),
					"cpuset.cpus",
					get_online("cpu")
		)))
	{
		cgroup_free(&cg);
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot set property \"cpuset.cpus\" for cpuset controller in cgroup \"/postgres\"")));
	}

	if ((rc = cgroup_set_value_string(
					cgroup_get_controller(cg, "cpuset"),
					"cpuset.mems",
					get_online("node")
		)))
	{
		cgroup_free(&cg);
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot set property \"cpuset.mems\" for cpuset controller in cgroup \"/postgres\"")));
	}

	/* update the cgroup "/postgres" in the kernel */
	if ((rc = cgroup_modify_cgroup(cg)))
	{
		cgroup_free(&cg);
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot modify cgroup \"/postgres\""),
				 errdetail("system error: %s", cgroup_strerror(rc)),
				 errhint("Configure the permissions of the \"/postgres\" cgroup properly before starting the server.")));
	}

	cgroup_free(&cg);

	/* our new cgroup will be called "/postgres/<pid>" */
	pid = getpid();
	snprintf(cg_name, 100, "/postgres/%d", pid);

	/* create a cgroup structure in memory */
	if (!(cg = cgroup_new_cgroup(cg_name)))
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot create struct cgroup \"%s\"", cg_name)));

	/* add the controllers we want */
	if (!(cg_memory = cgroup_add_controller(cg, "memory")))
	{
		cgroup_free(&cg);
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot add memory controller to cgroup \"%s\"", cg_name)));
	}
	if (!(cg_cpu = cgroup_add_controller(cg, "cpu")))
	{
		cgroup_free(&cg);
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot add cpu controller to cgroup \"%s\"", cg_name)));
	}
	if (!(cg_blkio = cgroup_add_controller(cg, "blkio")))
	{
		cgroup_free(&cg);
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot add blkio controller to cgroup \"%s\"", cg_name)));
	}
	if (!(cg_cpuset = cgroup_add_controller(cg, "cpuset")))
	{
		cgroup_free(&cg);
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot add cpuset controller to cgroup \"%s\"", cg_name)));
	}

	/* set "cpuset.cpus" and "cpuset.cpus" to good defaults */
	if ((rc = cgroup_add_value_string(
					cg_cpuset,
					"cpuset.cpus",
					get_online("cpu")
		)))
	{
		cgroup_free(&cg);
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot set property \"cpuset.cpus\" for cpuset controller in cgroup \"%s\"",
						cg_name)));
	}

	if ((rc = cgroup_add_value_string(
					cg_cpuset,
					"cpuset.mems",
					get_online("node")
		)))
	{
		cgroup_free(&cg);
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot set property \"cpuset.mems\" for cpuset controller in cgroup \"%s\"",
						cg_name)));
	}

	/* set permissions to the current uid and gid */
	if ((rc = cgroup_set_uid_gid(cg, getuid(), getgid(), getuid(), getgid())))
	{
		cgroup_free(&cg);
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot set permissions for cgroup \"%s\": %s",
						cg_name, cgroup_strerror(rc))));
	}

	cgroup_set_permissions(cg, 0755, 0644, 0644);

	/* add an atexit callback that will try to remove the cgroup */
	on_proc_exit(&on_exit_callback, PointerGetDatum(NULL));

	/* actually create the control group */
	if ((rc = cgroup_create_cgroup(cg, 0)))
	{
		cgroup_free(&cg);
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("error creating cgroup \"%s\": %s", cg_name, cgroup_strerror(rc))));
	}

	/* move the postmaster to the cgroup */
	if ((rc = cgroup_attach_task_pid(cg, pid)))
	{
		cgroup_free(&cg);
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot move process %d to cgroup \"%s\": %s",
						pid, cg_name, cgroup_strerror(rc))));
	}

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
		memory_limit_show
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
		swap_limit_show
	);

	EmitWarningsOnPlaceholders("pg_cgroups");

	cgroup_free(&cg);
}

/*
 * Get "online" parameters from the kernel.
 * "what" can be "cpu" or "node".
 */
char * const get_online(char * const what)
{
	static char value[100], path[100];
	int fd;
	ssize_t count;

	snprintf(path, 100, "/sys/devices/system/%s/online", what);

	errno = 0;
	if ((fd = open(path, O_RDONLY)) == -1)
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot open \"%s\": %m", path)));

	if ((count = read(fd, value, 99)) == -1)
	{
		(void) close(fd);
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot read from \"%s\": %m", path)));
	}

	(void) close(fd);

	value[(int)count] = '\0';
	return value;
}

/*
 * Read a 64-bit integer property from a kernel cgroup.
 */
int64_t cg_get_int64(char * const controller, char * const property)
{
	int rc;
	int64_t value;
	struct cgroup *cg;

	if (!(cg = cgroup_new_cgroup(cg_name)))
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot create struct cgroup \"%s\"", cg_name)));

	if ((rc = cgroup_get_cgroup(cg)))
	{
		cgroup_free(&cg);
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot read cgroup \"%s\" from kernel: %s",
						cg_name, cgroup_strerror(rc))));
	}

	if ((rc = cgroup_get_value_int64(
				cgroup_get_controller(cg, controller),
				property,
				&value
			)))
	{
		cgroup_free(&cg);
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot get \"%s\" for cgroup \"%s\": %s",
						property, cg_name, cgroup_strerror(rc))));
	}

	cgroup_free(&cg);

	return value;
}

/*
 * Write a 64-bit integer property to a kernel cgroup.
 */
void cg_set_int64(char * const controller, char * const property, int64_t value)
{
	int rc;
	struct cgroup *cg;

	if (!(cg = cgroup_new_cgroup(cg_name)))
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot create struct cgroup \"%s\"", cg_name)));

	if ((rc = cgroup_get_cgroup(cg)))
	{
		cgroup_free(&cg);
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot read cgroup \"%s\" from kernel: %s",
						cg_name, cgroup_strerror(rc))));
	}

	if ((rc = cgroup_set_value_int64(
					cgroup_get_controller(cg, controller),
					property,
					value
			)))
	{
		cgroup_free(&cg);
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot set \"%s\" for cgroup \"%s\": %s",
						property, cg_name, cgroup_strerror(rc))));
	}

	if ((rc = cgroup_modify_cgroup(cg)))
	{
		cgroup_free(&cg);
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot modify cgroup \"%s\": %s",
						cg_name, cgroup_strerror(rc))));
	}

	cgroup_free(&cg);
}

void
get_current_memory_limits(void)
{
	int64_t mem_value, mem_val_mb, swap_value, swap_val_mb;

	mem_value = cg_get_int64("memory", "memory.limit_in_bytes");
	swap_value = cg_get_int64("memory", "memory.memsw.limit_in_bytes");

	/* convert from bytes to MB */
	mem_val_mb = (mem_value == -1) ? -1 : (mem_value - 1) / 1048576 + 1;
	memory_limit = (int) ((mem_val_mb > INT_MAX / 2) ? -1 : mem_val_mb);
	swap_val_mb = (swap_value == -1) ? -1 : (swap_value - 1) / 1048576 + 1;
	swap_limit = (int) ((swap_val_mb > INT_MAX / 2) ? -1 : swap_val_mb);

	if (swap_limit != -1)
		swap_limit -= memory_limit;
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

	/* we have to adjust both memory and swap limit, so get both */
	get_current_memory_limits();

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
		cg_set_int64("memory", "memory.memsw.limit_in_bytes", swap_value);
		cg_set_int64("memory", "memory.limit_in_bytes", mem_value);
	}
	else
	{
		/* we have to lower the limit on memory + swap last */
		cg_set_int64("memory", "memory.limit_in_bytes", mem_value);
		cg_set_int64("memory", "memory.memsw.limit_in_bytes", swap_value);
	}

	memory_limit = newval;
}

const char *
memory_limit_show(void)
{
	static char value_str[100];

	get_current_memory_limits();

	snprintf(value_str, 100, "%d", memory_limit);
	return value_str;
}

void
swap_limit_assign(int newval, void *extra)
{
	int64_t swap_value, newtotal;

	get_current_memory_limits();

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

	cg_set_int64("memory", "memory.memsw.limit_in_bytes", swap_value);

	swap_limit = newval;
}

const char *
swap_limit_show(void)
{
	static char value_str[100];

	get_current_memory_limits();

	snprintf(value_str, 100, "%d", swap_limit);
	return value_str;
}

void on_exit_callback(int code, Datum arg)
{
	struct cgroup *cg;

	/* ignore all errors since we cannot report them anyway */
	if (!(cg = cgroup_new_cgroup(cg_name)))
		return;

	(void) cgroup_get_cgroup(cg);

	(void) cgroup_delete_cgroup(cg, 0);

	cgroup_free(&cg);
}
