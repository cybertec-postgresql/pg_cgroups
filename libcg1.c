#ifndef __linux__
#error "Linux control groups are only available on Linux"
#endif

#include "postgres.h"

#include "storage/fd.h"
#include "storage/ipc.h"
#include "utils/memutils.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pg_cgroups.h"

/* v11 did away with the third parameter of OpenTransientFile */
#if PG_VERSION_NUM < 110000
#define OpenTransFile(filename, fileflags) \
		OpenTransientFile((filename), (fileflags), S_IRUSR | S_IWUSR)
#else
#define OpenTransFile(filename, fileflags) \
		OpenTransientFile((filename), (fileflags))
#endif  /* PG_VERSION_NUM */

/*
 * static variables
 */

/* structure for information about cgroup controllers */
static struct {
	char *name;
	bool init;
	char *mountpoint;
} cgctl[MAX_CONTROLLERS] = {
	{"memory", false, NULL},
	{"cpu", false, NULL},
	{"blkio", false, NULL},
	{"cpuset", false, NULL}
};
/* postmaster PID */
static pid_t postmaster_pid;

/* default values for the parameters */
static char *def_cpus;
static char *def_memory_nodes;

/*
 * function prototypes
 */

static void check_controllers(void);
static void get_mountpoints(void);
static char * const get_online(char * const what);
static void cg_write_string(int controller, char * const cgroup, char * const parameter, char * const value);
static char *cg_read_string(int controller, char * const cgroup, char * const parameter, bool ignore_errors);
static void cg_move_process(char * const cgroup, char * const process, bool silent);
static void on_exit_callback(int code, Datum arg);

/*
 * static functions
 */

/* check if all required controllers are present */
void
check_controllers()
{
	FILE *cgfile;
	char *line = NULL;
	size_t size = 0;
	int i, len;

	errno = 0;

	/* check if all cgroup controllers exist */
	if ((cgfile = AllocateFile("/proc/cgroups", "r")) == NULL) {
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot open \"/proc/cgroups\": %m"),
				 errhint("Make sure that Linux Control Groups are supported by the kernel and activated.")));
	}

	/*
	 * This uses malloc(3) internally, which we shouldn't do in
	 * server code, but "getline" is so convenient.
	 * Make sure to free(2) "line"!
	 */
	while (getline(&line, &size, cgfile) != -1) {
		/* skip empty and comment lines */
		if (size < 1 || line[0] == '#')
			continue;

		/* set "init" true for any controller found */
		for (i=0; i<MAX_CONTROLLERS; ++i)
		{
			len = strlen(cgctl[i].name);
			if (strncmp(line, cgctl[i].name, len) == 0 && line[len] == '\t')
				cgctl[i].init = true;
		}

		/* reset for the next "getline" call */
		size = 0;
		if (line != NULL)
		{
			free(line);
			line = NULL;
		}
	}

	if (errno)
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("error reading from \"/proc/cgroups\": %m")));

	FreeFile(cgfile);

	/* check that all controllers are there */
	for (i=0; i<MAX_CONTROLLERS; ++i)
		if (!cgctl[i].init)
			ereport(FATAL,
					(errcode(ERRCODE_SYSTEM_ERROR),
					 errmsg("cgroup controller \"%s\" is not defined", cgctl[i].name),
					 errdetail("There is something wrong with your Linux Control Group setup.")));
}

/* find the mount points for the cgroup controllers*/
void
get_mountpoints()
{
	FILE *mntfile;
	struct mntent *mnt;
	char *p1, *p2, *fname;
	size_t len;
	int i;
	struct stat statbuf;

	/* open /proc/mounts, which contains the mounted file systems */
	if ((mntfile = AllocateFile("/proc/mounts", "r")) == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot open \"/proc/mounts\": %m"),
				 errdetail("There is something wrong with your Linux operating system.")));

	/* loop through all mounted file systems */
	while ((mnt = getmntent(mntfile)) != NULL) {
		/* skip if the type is wrong */
		if (strcmp(mnt->mnt_type, "cgroup") != 0)
			continue;

		/* find the controller name in the mount options */
		p1 = mnt->mnt_opts;
		while (p1 != NULL) {
			p2 = strchr(p1, ',');
			if (p2 == NULL)
				len = strlen(p1);
			else
				len = p2 - p1;

			/* if any of the options match, set the mount point */
			for (i=0; i<MAX_CONTROLLERS; ++i)
				if (strncmp(p1, cgctl[i].name, len) == 0
					&& (p1[len] == ',' || p1[len] == '\0'))
				{
					cgctl[i].mountpoint = MemoryContextStrdup(
												TopMemoryContext,
												mnt->mnt_dir);
					break;
				}

			p1 = p2 ? (p2 + 1) : NULL;
		}
	}

	FreeFile(mntfile);

	/* check that all cgroups are properly set up */
	for (i=0; i<MAX_CONTROLLERS; ++i)
	{
		if (cgctl[i].mountpoint == NULL)
			ereport(FATAL,
					(errcode(ERRCODE_SYSTEM_ERROR),
					 errmsg("no mount point found for cgroup controller \"%s\"", cgctl[i].name),
					 errdetail("There is something wrong with your Linux Control Group setup.")));

		/* there most be a "/postgres" cgroup there */
		fname = palloc(strlen(cgctl[i].mountpoint) + 10);
		sprintf(fname, "%s/postgres", cgctl[i].mountpoint);
		if (stat(fname, &statbuf) == -1 || !S_ISDIR(statbuf.st_mode))
			ereport(FATAL,
					(errcode(ERRCODE_SYSTEM_ERROR),
					 errmsg("no control group \"/postgres\" for the \"%s\" controller: %m", cgctl[i].name),
					 errhint("You have to create this control group as described in the pg_cgroup documentation.")));
		pfree(fname);
	}
};

/*
 * Get "online" parameters from the kernel.
 * "what" can be "cpu" or "node".
 * The result is a persistently palloc'ed string.
 */
char *
const get_online(char * const what)
{
	static char buf[100], *value, *path;
	int fd;
	ssize_t bytes, len;

	len = strlen(what) + 28;
	path = palloc(len);
	snprintf(path, len, "/sys/devices/system/%s/online", what);

	errno = 0;

	fd = OpenTransFile(path, O_RDONLY);

	pfree(path);

	if (fd == -1)
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot open \"%s\" for reading: %m", path)));

	value = MemoryContextAlloc(TopMemoryContext, 1);
	value[0] = '\0';
	while ((bytes = read(fd, buf, 100)) > 0)
	{
		size_t len = strlen(value);

		value = repalloc(value, len + bytes + 1);
		strncat(value, buf, bytes);
		value[len + bytes] = '\0';
	}

	if (errno)
	{
		pfree(value);

		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("error reading file \"%s\": %m", path)));
	}

	CloseTransientFile(fd);

	/* remove the trailing newline */
	value[strlen(value) - 1] = '\0';

	return value;
}

/*
 * Write a control group parameter.
 */
void
cg_write_string(int controller, char * const cgroup, char * const parameter, char * const value)
{
	char *path;
	int fd;

	path = palloc(strlen(cgctl[controller].mountpoint)
				  + strlen(cgroup)
				  + strlen(parameter) + 3);
	sprintf(path,
			"%s/%s/%s",
			cgctl[controller].mountpoint, cgroup, parameter);

	errno = 0;

	fd = OpenTransFile(path, O_WRONLY | O_TRUNC);

	if (fd == -1)
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("error opening file \"%s\" for writing: %m", path)));

	/*
	 * The attempt to write an empty string causes an error,
	 * so don't write anything in this case.
	 * The file is truncated on open anyway.
	 */
	if (strlen(value) > 0 && write(fd, value, strlen(value)) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("error writing file \"%s\": %m", path)));

	pfree(path);

	CloseTransientFile(fd);
}

/*
 * Read a control group parameter.
 * Returns a palloc'ed value.
 * If "ignore_errors" is "true", the function returns NULL if it encounters errors.
 */
char *
cg_read_string(int controller, char * const cgroup, char * const parameter, bool ignore_errors)
{
	char *result = NULL, *path, buf[1000];
	ssize_t bytes, total = 0;
	int fd;

	path = palloc(strlen(cgctl[controller].mountpoint)
				  + strlen(cgroup)
				  + strlen(parameter) + 3);
	sprintf(path,
			"%s/%s/%s",
			cgctl[controller].mountpoint, cgroup, parameter);

	errno = 0;

	fd = OpenTransFile(path, O_RDONLY | O_TRUNC);

	if (fd == -1)
	{
		if (ignore_errors)
			return NULL;
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYSTEM_ERROR),
					 errmsg("error opening file \"%s\" for reading: %m", path)));
	}

	while ((bytes = read(fd, buf, 1000)) > 0)
	{
		total += bytes;

		if (result)
			result = repalloc(result, total + 1);
		else
		{
			result = palloc(total + 1);
			result[0] = '\0';
		}

		strncat(result, buf, bytes);
	}
	if (errno)
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("error reading file \"%s\": %m", path)));

	pfree(path);

	CloseTransientFile(fd);

	return result;
}

/*
 * Add the processes to a Linux control group for all controllers.
 * "processes" contains the process IDs, separated by comma.
 * If "silent", ignore errors.
 */
void
cg_move_process(char * const cgroup, char * const process, bool silent)
{
	int i, fd;
	char *path;

	for (i=0; i<MAX_CONTROLLERS; ++i)
	{
		path = palloc(strlen(cgctl[i].mountpoint) + 30);
		sprintf(path, "%s/%s/tasks", cgctl[i].mountpoint, cgroup);

		fd = OpenTransFile(path, O_WRONLY);

		if (fd == -1)
		{
			if (silent)
			{
				CloseTransientFile(fd);
				continue;
			}

			ereport(ERROR,
					(errcode(ERRCODE_SYSTEM_ERROR),
					 errmsg("cannot open \"%s\" for writing: %m", path)));
		}

		if (write(fd, process, strlen(process) + 1) < 0)
		{
			if (silent)
			{
				CloseTransientFile(fd);
				continue;
			}

			ereport(ERROR,
					(errcode(ERRCODE_SYSTEM_ERROR),
					 errmsg("error writing file \"%s\": %m", path)));
		}

		pfree(path);

		CloseTransientFile(fd);
	}
}

void
on_exit_callback(int code, Datum arg)
{
	int i;
	char *path, *processes, *p, *q;

	/* "postmaster_pid" is shorter than 30 digits */
	path = palloc(40);
	sprintf(path, "postgres/%d", postmaster_pid);
	processes = cg_read_string(CONTROLLER_MEMORY, path, "tasks", false);
	pfree(path);

	/* we have to move the processes out of the control groups one by one */
	p = processes;
	while (*p != '\0')
	{
		q = strchr(p, '\n');
		*q = '\0';
		cg_move_process("postgres", p, true);
		p = q + 1;
	}

	pfree(processes);

	/* remove the control groups */
	for (i=0; i<MAX_CONTROLLERS; ++i)
	{
		/* "postmaster_pid" is shorter than 30 digits */
		path = palloc(strlen(cgctl[i].mountpoint) + 40);
		sprintf(path, "%s/postgres/%d", cgctl[i].mountpoint, postmaster_pid);
		(void) rmdir(path);
		pfree(path);
	}
}

/*
 * interface functions
 */

/*
 * Perform all the required initialization:
 * - find the mount points for the control groups
 * - configure the "/postgres" cgroup
 * - create a cgroup for this PostgreSQL instance
 * - move the instance to that cgroup
 * - register an "atexit" callback that will remove the cgroup at postmaster exit
 * - define the custom GUCs
 * - find out (and return) if the kernel has "memory.memsw.limit_in_bytes"
 */
void
cg_init(bool *cgroup_has_swap_param)
{
	char *path, *cgroup, pid_s[30], *memsw;
	int i;

	postmaster_pid = getpid();
	/* no process ID can be longer than 30 digits */
	snprintf(pid_s, 30, "%d", postmaster_pid);

	/* check that all required cgroup controllers are present */
	check_controllers();

	/* find the mount points for the cgroup controllers */
	get_mountpoints();

	/* register a callback that will clean up on postmaster exit */
	on_proc_exit(&on_exit_callback, PointerGetDatum(NULL));

	/* create a control group for this cluster */
	for (i=0; i<MAX_CONTROLLERS; ++i)
	{
		path = palloc(strlen(cgctl[i].mountpoint) + 31);
		sprintf(path, "%s/postgres/%d", cgctl[i].mountpoint, postmaster_pid);

		if (mkdir(path, 0700) == -1)
			ereport(FATAL,
					(errcode(ERRCODE_SYSTEM_ERROR),
					 errmsg("cannot create control group \"/postgres/%d\" for the \"%s\" controller: %m",
							postmaster_pid, cgctl[i].name),
					 errhint("You have to setup the \"/postgres\" control group as described in the pg_cgroup documentation.")));

		pfree(path);
	}

	/*
	 * Initialize cpuset.cpus and cpuset.mems to useful defaults.
	 * Otherwise we cannot add processes to these cgroups.
	 * We must do this on the "/postgres" cgroup first.
	 */
	cgroup = palloc(40);
	sprintf(cgroup, "postgres/%d", postmaster_pid);
	def_cpus = get_online("cpu");
	cg_write_string(CONTROLLER_CPUSET, "postgres", "cpuset.cpus", def_cpus);
	cg_write_string(CONTROLLER_CPUSET, cgroup, "cpuset.cpus", def_cpus);
	def_memory_nodes = get_online("node");
	cg_write_string(CONTROLLER_CPUSET, "postgres", "cpuset.mems", def_memory_nodes);
	cg_write_string(CONTROLLER_CPUSET, cgroup, "cpuset.mems", def_memory_nodes);

	/* set "cpu.cfs_period_us" to 100000 */
	cg_write_string(CONTROLLER_CPU, cgroup, "cpu.cfs_period_us", "100000");

	/*
	 * On kernels configured without CONFIG_MEMCG_SWAP_ENABLED,
	 * the "memory.memsw.limit_in_bytes" parameter is not available.
	 * Find out by trying to read the parameter.
	 */
	memsw = cg_read_string(CONTROLLER_MEMORY, "postgres", "memory.memsw.limit_in_bytes", true);
	if (memsw)
	{
		*cgroup_has_swap_param = true;
		pfree(memsw);
	}
	else
		*cgroup_has_swap_param = false;

	/* add the postmaster to the newly created cgroups */
	cg_move_process(cgroup, pid_s, false);

	pfree(cgroup);
}

void
cg_set_string(int controller, char * const parameter, char * const value)
{
	char cgroup[40];

	/* "postmaster_pid" cannot exceed 30 digits */
	sprintf(cgroup, "postgres/%d", postmaster_pid);

	cg_write_string(controller, cgroup, parameter, value);
}

void
cg_set_int64(int controller, char * const parameter, int64_t value)
{
	char str[25];	/* long enough for an int64 */

	snprintf(str, 25, "%" PRId64, value);

	cg_set_string(controller, parameter, str);
}

/* getter functions for the default values */
char
* const get_def_cpus(void)
{
	return def_cpus;
}

char *
const get_def_memory_nodes(void)
{
	return def_memory_nodes;
}
