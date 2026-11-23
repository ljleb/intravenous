# Early ivctl development commands

The initial C++ `ivctl` implementation exposed two development commands.
`ivctl cache server` configured and built the server with CMake. It also
accepted `-- COMMAND` to run a command with the build tree leased.
`ivctl cache scratch -- COMMAND` ran a command with a leased temporary
directory. It passed that directory in `IV_BUILD_SCRATCH_DIR`.

The repository now uses [development scripts](../development_run.md#use-the-managed-cmake-tree)
to build and stage the server. The launcher and benchmarks use a development
helper for the leases. `ivctl` provides cache inspection and cleanup.
