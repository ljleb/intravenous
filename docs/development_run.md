# Run Intravenous for development

From the repository root, enter the Nix development shell. Install the client
build dependencies once and again when the lockfile changes.

```sh
nix develop
npm --prefix vscode/client ci
```

Launch your regular VSCodium on an Intravenous project directory. Close that
folder first if it is already open in VSCodium.

```sh
nix develop --command ./scripts/run_dev.sh projects/simple_sine
```

Inside the development shell, run `./scripts/run_dev.sh projects/simple_sine`
directly. To use a CMake tree outside the managed cache, pass its directory
as the second argument. You can also use the **Run Intravenous Development** editor task.
The launcher uses `codium` from your PATH. Use `INTRAVENOUS_DEV_CODIUM` to
select a specific VSCodium executable.

The launcher configures a Release tree in the build cache on first use. It
runs the incremental `intravenous` build before it builds the extension.
A failed build or a server executable older than its staged inputs stops the launch.
Nix prepares the DSL precompiled header and built-in packages for that server
and stores them with its SDK and tools. The extension uses the matching server
from its bundle. Client-only changes can reuse the prepared server files.
Close the development window and run the launcher again to pick up a new revision.

VSCodium loads the extension in development mode and uses your regular editor
profile. It can open the dev window in the running editor. Shared lane state
stays in `iv_project.jsonl`.

For cursor wrapping and cursor return on Wayland, start VSCodium with XWayland
if your desktop provides an X11 display. An existing VSCodium process keeps its
current display backend.

```sh
INTRAVENOUS_DEV_XWAYLAND=1 nix develop --command ./scripts/run_dev.sh projects/simple_sine
```

The extension keeps its Nix store bundle alive while the dev window uses it.
After the session ends, `nix-collect-garbage` can remove the bundle and any
server or client files that other sessions no longer use. See the
[previous isolated launch workflow](./historical/isolated_dev_launch_before_shared_codium_2026-11-19.md).

If the server exits, the client tries up to three restarts with the same server
path. A run of at least 30 seconds resets that budget. Use
**Intravenous: Retry Server** to retry after the automatic attempts stop.
`INTRAVENOUS_DIR` and `intravenous.intravenousDir` still select an explicit
server directory when you run the extension separately.

## Use the managed CMake tree

To build the server on its own, run this command in the Nix shell.

```sh
python3 scripts/build_cache.py server
```

To use a Debug tree, pass `--build-type Debug` after `server`. To build other
targets or run tests, pass a command after `--`. It receives
`IV_SERVER_BUILD_DIR` and holds a lease on the tree for the command's duration.
A lease is a file lock that protects files in use.

```sh
python3 scripts/build_cache.py server -- sh -c 'cmake --build "$IV_SERVER_BUILD_DIR" -j16 && ctest --test-dir "$IV_SERVER_BUILD_DIR" -j16'
```

The managed tree contains `compile_commands.json`. If your editor
configuration names a previous build directory, point it to the managed tree
using the path in `IV_SERVER_BUILD_DIR`.

To run a command in temporary storage, use `python3 scripts/build_cache.py scratch -- COMMAND`.
The command receives the path in `IV_BUILD_SCRATCH_DIR`. The package build and
edit latency benchmarks use this storage by default. To keep a benchmark
workspace or report, give it a path outside the managed cache.

## Configure the build cache

The cache stores writable build files outside the checkout by default. It uses
`${XDG_CACHE_HOME:-$HOME/.cache}/intravenous/build-cache`. Set
`IV_BUILD_CACHE_DIR` to an absolute path to choose another location. Give the
launcher and the editor process the same setting if you use an override.

```text
build-cache/
  server/<checkout>/<build-profile>/
  project-packages/<project>/<package>/<build-profile>/
  shared-packages/<package-source>/<build-profile>/
  scratch/<run-id>/
  locks/
```

Keys include the canonical source paths and build profiles. A profile identifies
the toolchain and build configuration. Source edits reuse the CMake tree.
Package artifact sets have separate paths so active graphs can retain an older set.
Built-in package artifacts belong to the executable or Nix bundle that provides them.

To set your policy, create
`${XDG_CONFIG_HOME:-$HOME/.config}/intravenous/build-cache.json`. The launcher
and server read it when they collect the cache. Omitted fields use these defaults.

```json
{
  "thresholdGiB": 40,
  "targetGiB": 30,
  "minimumFreeGiB": 15,
  "maxIdleDays": 30
}
```

Use nonnegative whole numbers. Set `targetGiB` to a value at or below `thresholdGiB`.

## Garbage collection

The collector runs before and after each managed build, including failed builds.
Run `python3 scripts/build_cache.py gc` for a manual sweep. It lists the files
or directories it removed and the entries it kept, with a reason for each.
It also reports the remaining cache size and free disk space.

The report gives separate counts for build workspaces, package artifact sets,
and scratch directories. A removed workspace includes its contents. The
artifact counts cover sets inspected within workspaces that the collector kept.

Each sweep follows the same order.

1. Remove scratch whose lease is gone. Normal exits run a final sweep.
   A later sweep removes scratch left by a terminated process.
2. Remove superseded or incomplete package artifact sets whose leases are gone.
   Keep the latest successful set while its build entry stays in the cache.
   A failed build leaves the previous successful set available.
3. Remove build entries whose leases are gone and whose last use reaches
   `maxIdleDays`. The loader and launcher record use explicitly.
4. If the cache exceeds `thresholdGiB` or the filesystem has less than
   `minimumFreeGiB` free, evict the least recently used entries first.
   Stop once the cache reaches `targetGiB` and the disk meets the free-space floor,
   or when all remaining entries are in use.

Build commands hold a writer lock. Child processes inherit it when they access
the build tree. Loaded packages hold leases on their artifact sets and on the
CMake tree, which can contain linked libraries. The kernel releases a lease
when its last descriptor closes, including after a crash.

Package cache hits skip the build sweeps. For a package that needs compilation,
the loader keeps the requested tree leased during cleanup.

If cleanup cannot reach the free-space floor, the launcher or package loader
defers the new build. If space is available, live entries can keep the cache
above the threshold. These values govern cleanup at sweeps. A build can grow
beyond them while it runs.

The collector works on the managed writable cache. Project data, benchmark
reports saved elsewhere, and existing manually configured build directories
stay under their existing owners. Use `nix-collect-garbage` to reclaim
unreferenced Nix bundles separately.

See the [previous build locations](./historical/build_artifact_locations_before_managed_cache_2026-11-20.md).
