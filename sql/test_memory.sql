-- check the default settings
SHOW pg_cgroups.memory_limit;
SHOW pg_cgroups.swap_limit;

-- change only swap_limit (expect no effect)
ALTER SYSTEM SET pg_cgroups.swap_limit = 512;
SELECT pg_reload_conf();
SHOW pg_cgroups.swap_limit;

-- change only memory limit (should now change both)
ALTER SYSTEM SET pg_cgroups.memory_limit = 1024;
SELECT pg_reload_conf();
SHOW pg_cgroups.memory_limit;
SHOW pg_cgroups.swap_limit;

-- lower memory limit (should work)
ALTER SYSTEM SET pg_cgroups.memory_limit = 256;
SELECT pg_reload_conf();
SHOW pg_cgroups.memory_limit;
SHOW pg_cgroups.swap_limit;

-- raise memory limit (should work)
ALTER SYSTEM SET pg_cgroups.memory_limit = 512;
SELECT pg_reload_conf();
SHOW pg_cgroups.memory_limit;
SHOW pg_cgroups.swap_limit;

-- set swap limit to -1 (should work)
ALTER SYSTEM SET pg_cgroups.swap_limit = -1;
SELECT pg_reload_conf();
SHOW pg_cgroups.memory_limit;
SHOW pg_cgroups.swap_limit;

-- set swap limit to 0 (should work)
ALTER SYSTEM SET pg_cgroups.swap_limit = 0;
SELECT pg_reload_conf();
SHOW pg_cgroups.memory_limit;
SHOW pg_cgroups.swap_limit;

-- set memory limit to 0 (should fail)
ALTER SYSTEM SET pg_cgroups.memory_limit = 0;
SELECT pg_reload_conf();

-- reset all settings
ALTER SYSTEM RESET pg_cgroups.memory_limit;
ALTER SYSTEM RESET pg_cgroups.swap_limit;
SELECT pg_reload_conf();
