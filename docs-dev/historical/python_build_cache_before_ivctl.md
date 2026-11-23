# Python cache commands before ivctl

At revision `cb1cbde1`, [the development workflow](../development_run.md#use-the-managed-cmake-tree)
used `scripts/build_cache.py` as the cache entry point.

- `python3 scripts/build_cache.py server` built the server in the managed CMake tree.
- `python3 scripts/build_cache.py server -- COMMAND` ran a command with the tree leased.
- `python3 scripts/build_cache.py scratch -- COMMAND` leased a temporary directory.
- `python3 scripts/build_cache.py gc` ran and reported a sweep.

The server invoked the same script around package compilation. The script
selected server trees with truncated SHA-256 digests of their checkout paths
and build profiles.

`ivctl` replaces these commands with a native CLI. Its cache implementation
also links directly into the server.
