# Run Intravenous for development

From the repository root, enter the Nix development shell and build the server.
Install the client build dependencies into the checkout once, or again when its
lockfile changes.

```sh
nix develop
cmake -S . -B build-release -G Ninja -DJUCE_DIR="$JUCE_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target intravenous
npm --prefix vscode/client ci
```

Launch your regular VSCodium on an Intravenous project directory. Close that
folder first if it is already open in VSCodium.

```sh
nix develop --command ./scripts/run_dev.sh projects/simple_sine
```

Inside the development shell, run `./scripts/run_dev.sh projects/simple_sine`
directly. Pass a build directory as the second argument to use another completed
server build. You can also use the **Run Intravenous Development** editor task.
The launcher uses `codium` from your PATH. Use `INTRAVENOUS_DEV_CODIUM` to
select a specific VSCodium executable.

The launcher builds the extension and uses your completed server build.
Nix prepares the DSL precompiled header and built-in packages for that server
and stores them with its SDK and tools. The extension uses the matching server
from its bundle. Client-only changes can reuse the prepared server files.
Close the development window and run the launcher again to pick up a new revision.

VSCodium loads the extension in development mode and uses your regular editor
profile. It can open the dev window in the running editor. Project files such as
`.intravenous/graph-layout.json` and writable project package caches stay in
the project.

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
