MODULE_big = pg_cgroups
OBJS = pg_cgroups.o
DOCS = README.pg_cgroups
SHLIB_LINK = -lcgroup
REGRESS = test_memory test_blkio test_cpu

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
