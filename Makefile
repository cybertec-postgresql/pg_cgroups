MODULE_big = pg_cgroups
OBJS = pg_cgroups.o
EXTENSION = pg_cgroups
DATA = pg_cgroups--*.sql
DOCS = README.pg_cgroups
SHLIB_LINK = -lcgroup
REGRESS = 

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
