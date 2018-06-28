/*
 * Unfortunately I cannot test everything because I cannot
 * rely on the existence of a certain block device on
 * every Linux system.
 */

-- try several incorrect settings that should fail
ALTER SYSTEM SET pg_cgroups.read_bps_limit = '1024';
ALTER SYSTEM SET pg_cgroups.write_bps_limit = '8:0';
ALTER SYSTEM SET pg_cgroups.read_iops_limit = ':0 9210';
ALTER SYSTEM SET pg_cgroups.write_iops_limit = '100 9210';
ALTER SYSTEM SET pg_cgroups.read_bps_limit = '100: 9210';
ALTER SYSTEM SET pg_cgroups.write_iops_limit = '1:0 xyz';
