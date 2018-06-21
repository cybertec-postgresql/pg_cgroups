-- check the default settings (should both be -1)
SHOW pg_cgroups.memory_limit;
SHOW pg_cgroups.swap_limit;
SHOW pg_cgroups.oom_killer;

-- change swap_limit (will set the parameter, leave kernel value unlimited)
ALTER SYSTEM SET pg_cgroups.swap_limit = 512;
SELECT pg_reload_conf();
SELECT pg_sleep_for('0.3');
SHOW pg_cgroups.swap_limit;

-- change memory limit (should work)
ALTER SYSTEM SET pg_cgroups.memory_limit = 1024;
SELECT pg_reload_conf();
SELECT pg_sleep_for('0.3');
SHOW pg_cgroups.memory_limit;
SHOW pg_cgroups.swap_limit;

-- change swap_limit (should work)
ALTER SYSTEM SET pg_cgroups.swap_limit = 0;
SELECT pg_reload_conf();
SELECT pg_sleep_for('0.3');
SHOW pg_cgroups.swap_limit;

-- lower memory limit (should work)
ALTER SYSTEM SET pg_cgroups.memory_limit = 256;
SELECT pg_reload_conf();
SELECT pg_sleep_for('0.3');
SHOW pg_cgroups.memory_limit;
SHOW pg_cgroups.swap_limit;

-- raise memory limit (should work)
ALTER SYSTEM SET pg_cgroups.memory_limit = 512;
SELECT pg_reload_conf();
SELECT pg_sleep_for('0.3');
SHOW pg_cgroups.memory_limit;
SHOW pg_cgroups.swap_limit;

-- set swap limit to -1 (should work)
ALTER SYSTEM SET pg_cgroups.swap_limit = -1;
SELECT pg_reload_conf();
SELECT pg_sleep_for('0.3');
SHOW pg_cgroups.swap_limit;

-- set swap limit to 0 (should work)
ALTER SYSTEM SET pg_cgroups.swap_limit = 0;
SELECT pg_reload_conf();
SELECT pg_sleep_for('0.3');
SHOW pg_cgroups.swap_limit;

-- set memory limit to 0 (should fail)
ALTER SYSTEM SET pg_cgroups.memory_limit = 0;

-- disable OOM killer (should work)
ALTER SYSTEM SET pg_cgroups.oom_killer = off;
SELECT pg_reload_conf();
SELECT pg_sleep_for('0.3');
SHOW pg_cgroups.oom_killer;

-- reset all settings
ALTER SYSTEM RESET pg_cgroups.memory_limit;
ALTER SYSTEM RESET pg_cgroups.swap_limit;
ALTER SYSTEM RESET pg_cgroups.oom_killer;
SELECT pg_reload_conf();
SELECT pg_sleep_for('0.3');
SHOW pg_cgroups.memory_limit;
SHOW pg_cgroups.swap_limit;
SHOW pg_cgroups.oom_killer;
