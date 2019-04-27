Linux Control Groups for PostgreSQL
===================================

`pg_cgroups` is a module that allows you to run a PostgreSQL database cluster
in a Linux Control Group (cgroup) and set the cgroup parameters as PostgreSQL
configuration parameters.

This enables you to limit the operating system resources for the cluster.

Installation
============

Make sure that you have the PostgreSQL headers and the extension
building infrastructure installed.  If you do not build PostgreSQL
from source, this is done by installing a `*-devel` or `*-dev`
package.

Check that the correct `pg_config` is found on the `PATH`.  
Then build and install `pg_cgroups` with

    make
    sudo make install

Then you must add `pg_cgroups` to `shared_preload_libraries` and restart
the PostgreSQL server process, but make sure that you have completed the
setup as described below, or PostgreSQL will not start.

Setup
=====

As user `root`, create the file `/etc/cgconfig.conf` with the following
content:

    group postgres {
        perm {
            task {
                uid = postgres;
                gid = postgres;
                fperm = 644;
            }
            admin {
                uid = postgres;
                gid = postgres;
                dperm = 755;
                fperm = 644;
            }
        }

        memory {
        }

        blkio {
        }

        cpu {
        }

        cpuset {
        }
    }

Here `postgres` is the PostgreSQL operating system user.

Then make sure that cgroups are initialized and `/etc/cgconfig.conf`
is loaded.  How this is done will depend on the distribution.
On RedHat-based systems, you would do the following:

    yum install -y libcgroup libcgroup-tools
    systemctl enable cgconfig
    systemctl start cgconfig

If PostgreSQL is automatically started during system startup, make sure
that cgroups are configured before PostgreSQL is started.
With `systemd`, you can do that by adding an `After` and a `Requires`
option to the `[Unit]` section of the PostgreSQL service file.

Usage
=====

PostgreSQL will automatically create a cgroup called `/postgres/<pid>` (where
`<pid>` is the postmaster process ID) for the following controllers:

- memory
- cpu
- blkio
- cpuset

Then it will add itself to this cgroup so that all PostgreSQL processes
get to run under that cgroup.  The cgroup is deleted when PostgreSQL is
shut down.

You can configure limits for various operating system resources by setting
configuration parameters in `postgresql.conf` or with `ALTER SYSTEM`.

You should also avoid modifying the cgroup parameters outside of PostgreSQL.
This will work, but then the configuration parameters won't contain the
correct setting.

`pg_cgroups` tries to check the parameter values for validity, but be careful
because an incorrect parameter setting will cause PostgreSQL to stop.

The parameters are:

Memory parameters
-----------------

- `pg_cgroups.memory_limit` (type `integer`, unit MB, default value -1)

  This corresponds to the cgroup memory parameter
  `memory.limit_in_bytes` and limits the amount of RAM available.

  The parameter can be positive or -1 for "no limit".

  Once `memory_limit` plus `swap_limit` is exhausted, the `oom_killer`
  parameter determines what will happen.

- `pg_cgroups.swap_limit` (type `integer`, unit MB, default value -1)

  This configures the cgroup memory parameter `memory.memsw.limit_in_bytes`
  and limits the available swap space
  (note, however, that while `memory.memsw.limit_in_bytes` limits the sum of
  memory and swap space, `pg_cgroups.swap_limit` limits *only* the swap space).

  This parameter can be 0, positive or -1 for "no limit".

  Once `memory_limit` plus `swap_limit` is exhausted, the `oom_killer`
  parameter determines what will happen.

- `pg_cgroups.oom_killer` (type `boolean`, default value `on`)

  This parameter configures what will happen if the limit on memory and swap
  space is exhausted.  If set to `on`, the Linux out-of-memory killer will
  kill PostgreSQL processes, otherwise execution is suspended until some
  memory is freed (which may never happen).

Block-I/O parameters
--------------------

  For all these parameters, the format of the entries is `major:minor limit`,
  where `major` and `minor` are the device major and minor numbers,
  and `limit` is a number (bytes or number of I/O operations).

  To limit I/O on several devices, use several such entries, separated by
  a comma.

  For example, if I want to limit I/O on the device `/dev/mapper/home`,
  you first find out what device that actually is:

      $ readlink -e /dev/mapper/home
      /dev/dm-2

  Then you find out the major and minor numbers:

      $ ls -l /dev/dm-2
      brw-rw---- 1 root disk 253, 2 Jun 21 12:13 /dev/dm-2

  So in this case, you would use an entry like `253:2 1048576` if you want to
  limit I/O to 1MB per second.

  To remove a limit with `ALTER SYSTEM`, you have to set it to 0 explicitly,
  as in `253:2 0`.
  Using `ALTER SYSTEM RESET` or setting the limit to an empty string won't
  change the limit (this is how Linux control groups are implemented).
  However, setting the limit to an empty string and restarting the server
  will work, since the cgroup is deleted and re-created in this case.

- `pg_cgroups.read_bps_limit` (type `text`, default empty)

  This corresponds to the cgroup blkio parameter
  `blkio.throttle.read_bps_device` and limits the amount of bytes that can
  be read per second.

- `pg_cgroups.write_bps_limit` (type `text`, default empty)

  This corresponds to the cgroup blkio parameter
  `blkio.throttle.write_bps_device` and limits the amount of bytes that can
  be written per second.

- `pg_cgroups.read_iops_limit` (type `text`, default empty)

  This corresponds to the cgroup blkio parameter
  `blkio.throttle.read_iops_device` and limits the number of read I/O
  operations that can be performed per second.

- `pg_cgroups.write_iops_limit` (type `text`, default empty)

  This corresponds to the cgroup blkio parameter
  `blkio.throttle.write_iops_device` and limits the number of write I/O
  operations that can be performed per second.

CPU parameters
--------------

- `pg_cgroups.cpu_share` (type `integer`, default -1)

  This corresponds to the cgroup cpu parameter `cpu.cfs_quota_us` and defines
  the percentage of CPU bandwidth that can be used by PostgreSQL.
  The unit is 1/1000 of a percent, so 100000 stands for 100% of one CPU core.
  The minimum value is 1000, which stands for 1%.

  The default value -1 means &ldquo;no limit&rdqo;.

  To allow PostgreSQL to use more than one CPU fully, set the parameter to
  a value greater than 100000.

Diagnostic parameter
--------------------

- `pg_cgroups.version` (type `text`)

  This parameter shows the current version of `pg_cgroups` and can only
  be read.

NUMA parameters
---------------

These parameters limit the CPUs and memory nodes that can be used by PostgreSQL.
Setting these parameters usually only makes sense on [NUMA][1] architectures.
Use `numactl --hardware` so see your machine's NUMA configuration.

If you restrict PostgreSQL to run on the CPUs that belong to one memory node,
you should also restrict memory usage to that node and vice versa, so that
PostgreSQL only needs to access node-local memory.

All these parameters take the form of a comma separated list of zero based
numbers or number ranges, like `0`, `0-3` or `4,7-9`.

- `pg_cgroups.memory_nodes` (`text`, defaults to all online nodes)

  This corresponds to the cgroup parameter `cpuset.mems` and defines the
  memory nodes that PostgreSQL can use.

- `pg_cgroups.cpus` (`text`, defaults to all online CPUs)

  This corresponds to the cgroup parameter `cpuset.cpus` and defines the
  CPUs that PostgreSQL can use.

 [1]: https://en.wikipedia.org/wiki/Non-uniform_memory_access

Support
=======

You can
[open an issue](https://github.com/cybertec-postgresql/pg_cgroups/issues)
on Github if you have questions or problems.

For professional support, contact
[Cybertec](https://www.cybertec-postgresql.com).

Make sure you report which version you are using.
The version can be found with this SQL command:

    SHOW pg_cgroups.version;
