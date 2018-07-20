-- check the default settings
SHOW pg_cgroups.cpus;
SHOW pg_cgroups.memory_nodes;

-- test some incorrect settings
ALTER SYSTEM SET pg_cgroups.cpus = '-1';
ALTER SYSTEM SET pg_cgroups.cpus = '0-0-0';
ALTER SYSTEM SET pg_cgroups.cpus = '0,1,1-0';
ALTER SYSTEM SET pg_cgroups.cpus = '10000';
ALTER SYSTEM SET pg_cgroups.cpus = '1000000';
ALTER SYSTEM SET pg_cgroups.cpus = ',1';
ALTER SYSTEM SET pg_cgroups.cpus = '0-1,';

-- set the available CPUs
ALTER SYSTEM SET pg_cgroups.cpus = '0';
SELECT pg_reload_conf();
SELECT pg_sleep_for('0.3');
SHOW pg_cgroups.cpus;

-- set the available memory nodes
ALTER SYSTEM SET pg_cgroups.memory_nodes = '0';
SELECT pg_reload_conf();
SELECT pg_sleep_for('0.3');
SHOW pg_cgroups.memory_nodes;

-- reset
ALTER SYSTEM RESET pg_cgroups.cpus;
ALTER SYSTEM RESET pg_cgroups.memory_nodes;
SELECT pg_reload_conf();
SELECT pg_sleep_for('0.3');
SHOW pg_cgroups.cpus;
SHOW pg_cgroups.memory_nodes;
