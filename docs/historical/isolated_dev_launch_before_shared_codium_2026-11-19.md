# Run Intravenous for development

Archived on 2026-11-19 from `docs/development_run.md` when the launcher began using the regular VSCodium profile and placing the prepared server files in the Nix store. See the [current workflow](../development_run.md).

From the repository root, enter the Nix development shell and build the server.
Install the client build dependencies into the checkout once, or again when its
lockfile changes.

```sh
nix develop
cmake -S . -B build-release -G Ninja -DJUCE_DIR="$JUCE_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target intravenous
npm --prefix vscode/client ci
```

Launch VSCodium on an Intravenous project directory.

```sh
nix develop --command ./scripts/run_dev.sh projects/simple_sine
```

Inside the development shell, run `./scripts/run_dev.sh projects/simple_sine`
directly. Pass a build directory as the second argument to use another completed
server build. You can also use the **Run Intravenous Development** editor task.
Set `INTRAVENOUS_DEV_CODIUM` to an absolute executable path to use a different
VSCodium version from the one in the shell.

The launcher builds the extension and copies the finished server and its tools
into a single Nix store path. Rebuilding the server or editing the client in the
checkout leaves the running session on its original files. Close the development
window and run the launcher again to pick up a new revision.

The editor uses a temporary profile and loads the extension in development
mode. The launcher prepares the DSL precompiled header and built-in package in a
writable directory for the session and removes that directory on normal exit.
Each session has its own editor preferences. Project files, including
`.intravenous/graph-layout.json`, stay in the project.

The launcher controls the editor's lifetime. To use cursor wrapping and cursor
return on Wayland, start the session with XWayland if your desktop provides an
X11 display.

```sh
INTRAVENOUS_DEV_XWAYLAND=1 nix develop --command ./scripts/run_dev.sh projects/simple_sine
```

Nix on Linux keeps the store snapshot while the launcher or editor holds it.
After the session ends, `nix-collect-garbage` can remove old snapshots.

If the server exits, the client tries up to three restarts with the same server
path. A run of at least 30 seconds resets that budget. Use
**Intravenous: Retry Server** to retry after the automatic attempts stop.
`INTRAVENOUS_DIR` and `intravenous.intravenousDir` still select an explicit
server directory when you run the extension separately.
