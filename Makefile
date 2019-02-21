MODULE_big = pg_cgroups
OBJS = pg_cgroups.o libcg1.o
DOCS = README.pg_cgroups
REGRESS = test_memory test_blkio test_cpu test_cpuset

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
