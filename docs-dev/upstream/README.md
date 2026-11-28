# Chromium Wayland pointer position patch

This experiment records one way to correct Chromium's Wayland pointer lock.
The graph editor's Mutter-owned drag path runs with stock VSCodium. See the
[current setup](../development_run.md#native-wayland-cursor-control).

## Build a separate VSCodium on NixOS

The [Nix recipe](../../scripts/lib/wayland_codium.nix) compiles Electron 43.7.7
with the pointer fix and pairs it with the installed VSCodium's application
files. It checks the Electron version, Chromium version, and native Node
module ABI against the installed editor. The first build compiles Chromium.
The development launcher can reuse the resulting editor for later launches.

Run these commands from the repository root on the NixOS host.

```sh
codium_bin=$(readlink -f "$(command -v codium)")
nix build --file scripts/lib/wayland_codium.nix \
  --argstr codiumRoot "${codium_bin%/bin/codium}" \
  --out-link /tmp/iv-wayland-codium

INTRAVENOUS_DEV_CODIUM=/tmp/iv-wayland-codium/bin/codium \
  nix develop --command ./scripts/run_dev.sh ~/music/projects/simple_sine
```

The packaged launcher sets `IV_WAYLAND_CURSOR_RETURN=1` and uses a separate
VSCodium profile under `$XDG_CONFIG_HOME/intravenous-wayland-codium`, defaulting
to `~/.config` as the config root. Enable the GNOME companion before trying
cursor return. See the [desktop setup](../development_run.md).

## Apply the patch in an Electron checkout

Electron 43.7.7 pins Chromium 150.0.7871.250. After syncing the Electron
checkout, apply the patch from the Chromium `src` directory.
[Electron's dependency pin](https://github.com/electron/electron/blob/v43.7.7/DEPS#L2-L6)

```sh
git apply --check /path/to/intravenous/docs-dev/upstream/chromium-150-wayland-pointer-position.patch
git apply /path/to/intravenous/docs-dev/upstream/chromium-150-wayland-pointer-position.patch
```

Build Chromium's `ozone_unittests` in the Electron source tree and run the
patched Wayland test. The test covers the first click without new pointer
motion, absolute motion received during lock, and a compositor move after
unlock.

```sh
autoninja -C out/Testing ozone_unittests
out/Testing/ozone_unittests --gtest_filter='*PointerButtonUsesAbsoluteLocationAfterUnlock*'
```

Use [Electron's GN build instructions](https://www.electronjs.org/docs/latest/development/build-instructions-gn)
for the checkout and build directory. The application integration and live
verification status are recorded in the [Vue client stages](../vue_client_implementation_stages.md#native-wayland-cursor-return-status-2026-11-27).
