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
static int memory_limit = -1;

/* exported function declarations */
extern void _PG_init(void);

/* static functions declarations */
static char * const get_online_cpus(void);
static char * const get_online_nodes(void);
static int64_t cg_get_int64(char * const controller, char * const property);
static void cg_set_int64(char * const controller, char * const property, int64_t value);
static bool memory_limit_check(int *newval, void **extra, GucSource source);
static void memory_limit_assign(int newval, void *extra);
static const char *memory_limit_show(void);
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

	/* we must set reasonable properties for certain parameters */
	if ((rc = cgroup_add_value_string(
					cg_cpuset,
					"cpuset.cpus",
					get_online_cpus()
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
					get_online_nodes()
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
	if ((rc = cgroup_create_cgroup(cg, false)))
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
		INT_MAX,
		PGC_SIGHUP,
		GUC_UNIT_MB,
		memory_limit_check,
		memory_limit_assign,
		memory_limit_show
	);

	EmitWarningsOnPlaceholders("pg_cgroups");

	cgroup_free(&cg);
}

#define CPUFILE "/sys/devices/system/cpu/online"

/*
 * Get the online CPUs from the kernel.
 */
char * const get_online_cpus(void)
{
	static char cpus[100];
	int fd;
	ssize_t count;

	errno = 0;
	if ((fd = open(CPUFILE, O_RDONLY)) == -1)
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot open \"%s\": %m", CPUFILE)));

	if ((count = read(fd, cpus, 99)) == -1)
	{
		(void) close(fd);
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot read from \"%s\": %m", CPUFILE)));
	}

	(void) close(fd);

	cpus[(int)count] = '\0';
	return cpus;
}

#define NODEFILE "/sys/devices/system/node/online"

/*
 * Get the online NUMA nodes from the kernel.
 */
char * const get_online_nodes(void)
{
	static char nodes[100];
	int fd;
	ssize_t count;

	errno = 0;
	if ((fd = open(NODEFILE, O_RDONLY)) == -1)
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot open \"%s\": %m", NODEFILE)));

	if ((count = read(fd, nodes, 99)) == -1)
	{
		(void) close(fd);
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot read from \"%s\": %m", NODEFILE)));
	}

	(void) close(fd);

	nodes[(int)count] = '\0';
	return nodes;
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
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot create struct cgroup \"%s\"", cg_name)));

	if ((rc = cgroup_get_cgroup(cg)))
	{
		cgroup_free(&cg);
		ereport(FATAL,
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
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot create struct cgroup \"%s\"", cg_name)));

	if ((rc = cgroup_get_cgroup(cg)))
	{
		cgroup_free(&cg);
		ereport(FATAL,
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
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot set \"%s\" for cgroup \"%s\": %s",
						property, cg_name, cgroup_strerror(rc))));
	}

	if ((rc = cgroup_modify_cgroup(cg)))
	{
		cgroup_free(&cg);
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot modify cgroup \"%s\": %s",
						cg_name, cgroup_strerror(rc))));
	}

	cgroup_free(&cg);
}

bool
memory_limit_check(int *newval, void **extra, GucSource source)
{
	if (*newval == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"pg_cgroups.memory_limit\" must be -1 or positive")));
	return true;
}

void
memory_limit_assign(int newval, void *extra)
{
	int64_t value;

	/* convert from MB to bytes */
	value = (newval == -1) ? -1 : newval * (int64_t)1048576;

	cg_set_int64("memory", "memory.limit_in_bytes", value);
}

const char *
memory_limit_show(void)
{
	int64_t value;
	static char value_str[100];

	value = cg_get_int64("memory", "memory.limit_in_bytes");

	/* convert from bytes to MB */
	memory_limit = (int) ((value == -1) ? -1 : (value - 1) / 1048576 + 1);
	snprintf(value_str, 100, "%d", memory_limit);
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
