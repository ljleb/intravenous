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

The Nix development shell provides `ivctl` for cache inspection and cleanup.
The development bundle includes its own copy at `bin/ivctl`. To run the tool
outside the development shell, use `nix run .#ivctl -- cache status`.

From the checkout root, run this script in the Nix shell to build the server
with CMake in a managed directory.

```sh
./scripts/build_server.sh
```

To use a Debug tree, pass `--build-type Debug`. To build other targets or run
tests, pass a command after `--`. The script sets `IV_SERVER_BUILD_DIR` and
holds a lease on the tree until the command exits. A lease is a file lock
that protects files in use.

```sh
./scripts/build_server.sh -- sh -c 'cmake --build "$IV_SERVER_BUILD_DIR" -j16 && ctest --test-dir "$IV_SERVER_BUILD_DIR" -j16'
```

The managed tree contains `compile_commands.json`. If your editor
configuration names a previous build directory, point it to the managed tree
using the path in `IV_SERVER_BUILD_DIR`.

If you already configured `build-release` in the checkout, run
`./scripts/test_release.sh -j16` to build it and run its tests.
The benchmark commands are under [benchmark/scripts](../benchmark/scripts/).

The launcher stages the development bundle in scratch storage. The package
build and edit latency benchmarks also use it for temporary workspaces.
The development tools give these commands their scratch path in
`IV_BUILD_SCRATCH_DIR`. To keep a benchmark workspace or report, give it a
path outside the managed cache.

See the [early development commands](./historical/ivctl_development_commands.md).

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

Run `ivctl cache config` to see the effective policy in a table of bytes and
days. Use `-h` for binary sizes such as `40G` and `-H` for decimal sizes
such as `42.95GB`. The table rounds scaled sizes to two decimal
places. Use these flags only to display the policy.

Use `set` to write one or more settings at once. Use `reset` with key
names to restore those defaults. To restore all defaults, run
`ivctl cache config reset` without key names.

```sh
ivctl cache config set thresholdBytes=20G targetBytes=10.5GB maxIdleDays=14
ivctl cache config reset maxIdleDays
```

A size letter on its own or followed by `iB` uses powers of 1024. A size letter
followed by `B`, such as `GB`, uses powers of 1000. Suffixes are case-insensitive.
For example, `10.5g`, `10.5G`, and `10.5GiB` all give 11,274,289,152 bytes.
`10.5GB` gives 10,500,000,000 bytes. A value without a suffix is in bytes.
If the result includes a fraction of a byte, it rounds to the nearest byte.
A half-byte rounds up. `maxIdleDays` takes a whole number of days.

The settings file is
`${XDG_CONFIG_HOME:-$HOME/.config}/intravenous/build-cache.json`. You can
also edit it directly. The launcher and server read it on their next sweep.
The file stores sizes as whole bytes. Omitted fields use these defaults.

```json
{
  "thresholdBytes": 42949672960,
  "targetBytes": 32212254720,
  "minimumFreeBytes": 16106127360,
  "maxIdleDays": 30
}
```

Use nonnegative whole numbers in the file. Set `targetBytes` at or below
`thresholdBytes`. The [previous GiB keys](./historical/build_cache_gib_settings.md)
can be replaced with the byte keys above.

## Garbage collection

The collector runs before and after each managed build, including failed builds.
Run `ivctl cache path` to print the cache root. Run `ivctl cache status` to list
the cache contents, which workspaces are in use, and how much space they use in
total. These commands work before the cache exists.

Run `ivctl cache gc` for a manual sweep. It lists what it removed and what it
kept, with a reason for each decision. It also reports the remaining cache
size and free disk space. Run `ivctl cache gc --dry-run` to preview those
decisions. The preview estimates free space from allocated file blocks and
leaves the cache untouched.

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
4. If the cache exceeds `thresholdBytes` or the filesystem has less than
   `minimumFreeBytes` free, evict the least recently used entries first.
   Stop once the cache reaches `targetBytes` and the disk meets the free-space floor,
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

`ivctl` uses a new key for server build trees. The first server build with it
uses a fresh tree. The collector still sees the previous trees and applies
the same policy to them. See the [previous Python cache commands](./historical/python_build_cache_before_ivctl.md).
