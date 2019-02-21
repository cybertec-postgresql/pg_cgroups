-- allow 50% of the availabe CPU
ALTER SYSTEM SET pg_cgroups.cpu_share = 50000;
SELECT pg_reload_conf();
SELECT pg_sleep_for('0.3');
SHOW pg_cgroups.cpu_share;

-- reset
ALTER SYSTEM RESET pg_cgroups.cpu_share;
SELECT pg_reload_conf();
