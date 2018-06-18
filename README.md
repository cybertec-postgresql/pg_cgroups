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

    yum install -y libcgroup libcgroup-devel libcgroup-tools
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
the following parameters in `postgresql.conf` are with `ALTER SYSTEM`.

Memory parameters
-----------------

- `pg_cgroups.memory_limit` (`integer`, unit MB, default value -1)

  This corresponds to the cgroup memory parameter
  `memory.limit_in_bytes` and limits the amount of RAM available.

  The parameter can be positive or -1 for "no limit".

  Once `memory_limit` plus `swap_limit` is exhausted, the `oom_killer`
  parameter determines what will happen.

- `pg_cgroups.swap_limit` (`integer`, unit MB, default value -1)

  This configures the cgroup memory parameter `memory.memsw.limit_in_bytes`
  and limits the available swap space
  (note, however, that while `memory.memsw.limit_in_bytes` limits the sum of
  memory and swap space, `pg_cgroups.swap_limit` limits *only* the swap space).

  This parameter can be 0, positive or -1 for "no limit".

  Once `memory_limit` plus `swap_limit` is exhausted, the `oom_killer`
  parameter determines what will happen.

- `pg_cgroups.oom_killer` ('boolean`, default value `on`)

  This parameter configures what will happen if the limit on memory and swap
  space is exhausted.  If set to `on`, the Linux out-of-memory killer will
  kill PostgreSQL processes, otherwise execution is suspended until some
  memory is freed (which may never happen).
