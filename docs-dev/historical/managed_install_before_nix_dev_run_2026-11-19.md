# Build and install Intravenous

Archived on 2026-11-19 from `docs-dev/server_installation.md` when the development
launcher replaced the local installer. See the [current workflow](../development_run.md).

Run these commands from the repository root in the development shell. The local
installer uses `flock` for Linux file locks.

```sh
cmake --build build-release --target intravenous --parallel 16
./scripts/install_client.sh build-release
```

Use **Build Intravenous** to build the server. Use **Install Intravenous** to
install the extension and its server in one action. For client edits, run the
installer with your existing server build. The installer also accepts a separate
installation directory as its second argument.

Each release contains the executable, builder library, package tools, and DSL
headers. The installer prepares the DSL precompiled header and built-in package
at their final paths. It changes `current` after VSCodium accepts the extension.
If preparation or extension installation fails, the current server release
stays selected. See the [previous workflow](./separate_server_and_client_installation_2026-11-19.md).

The extension pins a release on its first server launch. The server also holds
a lock until it exits. The operating system releases a process's locks when
it crashes. Installation, release selection, and clean extension shutdown
remove old releases without locks. An old release whose last process crashes
can be removed on the next installation or release selection.

If a running server exits, the extension tries up to three restarts with its
pinned release. A run of at least 30 seconds resets that budget. Use
**Intravenous: Retry Server** to try the pinned release again. Reload the window
to select the current release. The `INTRAVENOUS_DIR` environment variable and
the `intravenous.intravenousDir` setting still allow an explicit server path.
