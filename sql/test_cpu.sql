-- check the default settings
SHOW pg_cgroups.cpu_share;

-- this should fail
ALTER SYSTEM SET pg_cgroups.cpu_share = 0;

-- change to the minimum value
ALTER SYSTEM SET pg_cgroups.cpu_share = 50000;
SELECT pg_reload_conf();
SELECT pg_sleep_for('0.3');
SHOW pg_cgroups.cpu_share;

-- reset
ALTER SYSTEM RESET pg_cgroups.cpu_share;
SELECT pg_reload_conf();
SELECT pg_sleep_for('0.3');
SHOW pg_cgroups.cpu_share;
