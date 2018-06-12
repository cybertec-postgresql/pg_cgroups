#ifndef __linux__
#error "Linux control groups are only available on Linux"
#endif

#include "postgres.h"
#include "fmgr.h"

#include <libcgroup.h>
#include <limits.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>

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

	/*
	 * Create a cgroup for this cluster.
	 * First, we have to create an object that represents the group
	 * that we want to create.
	 */
	pid = getpid();
	snprintf(cg_name, 100, "/postgres/%d", pid);

	if (!(cg = cgroup_new_cgroup(cg_name)))
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot create struct cgroup \"%s\"", cg_name)));
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

	/* actually create the control group */
	if ((rc = cgroup_create_cgroup(cg, false)))
	{
		cgroup_free(&cg);
		ereport(FATAL,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("error creating cgroup \"%s\": %s", cg_name, cgroup_strerror(rc))));
	}

	/* add an atexit callback that will remove the cgroup */
	on_proc_exit(&on_exit_callback, PointerGetDatum(NULL));

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

	EmitWarningsOnPlaceholders("pg_cgroups");

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

	if (!(rc = cgroup_set_value_int64(
					cgroup_get_controller(cg, "memory"),
					"memory.limit_in_bytes",
					(newval == -1) ? -1 : newval * (int64_t)1048576
			)))
		rc = cgroup_modify_cgroup(cg);

	cgroup_free(&cg);

	if (rc)
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot set \"memory.limit_in_bytes\" for cgroup \"%s\": %s",
						cg_name, cgroup_strerror(rc))));
}

const char *
memory_limit_show(void)
{
	int rc;
	int64_t limit;
	static char limit_str[100];
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

	rc = cgroup_get_value_int64(
				cgroup_get_controller(cg, "memory"),
				"memory.limit_in_bytes",
				&limit
			);

	cgroup_free(&cg);

	if (rc)
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("cannot get \"memory.limit_in_bytes\" for cgroup \"%s\": %s",
						cg_name, cgroup_strerror(rc))));

	memory_limit = (int)((limit = -1) ? -1 : (limit - 1) / 1048576 + 1);
	snprintf(limit_str, 100, "%d", memory_limit);
	return limit_str;
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
