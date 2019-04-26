/* cgroup controllers we use */

#define MAX_CONTROLLERS 4

#define CONTROLLER_MEMORY 0
#define CONTROLLER_CPU    1
#define CONTROLLER_BLKIO  2
#define CONTROLLER_CPUSET 3

/* defined in pg_cgrops.c */
extern const char * const PG_CGROUPS_VERSION;
extern void _PG_init(void);

/* defined in libcg1.c */
extern void cg_init(void);
extern char * const get_def_cpus(void);
extern char * const get_def_memory_nodes(void);
extern void cg_set_string(int controller, char * const parameter, char * const value);
extern void cg_set_int64(int controller, char * const parameter, int64_t value);
