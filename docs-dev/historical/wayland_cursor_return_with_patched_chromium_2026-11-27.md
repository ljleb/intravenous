# Wayland cursor return with patched Chromium

This development setup was superseded on 2026-11-27 by Mutter-owned drag
capture. It comes from [development_run.md](../development_run.md). See the
[active setup](../development_run.md#native-wayland-cursor-control) and the
[Chromium patch experiment](../upstream/README.md).

The launcher uses the display backend selected by VSCodium. The native
helper supports X11 cursor return. Native Wayland cursor return requires
GNOME Shell 50, the GNOME companion extension, and a VSCodium build with the
[Chromium pointer position fix](./upstream/README.md). See the
[retired XWayland launch options](./historical/editor_xwayland_restart_removed_2026-11-27.md).

To install the GNOME companion for your user account, run these commands from
the repository root. If GNOME Shell cannot find the new extension, log out and
back in before enabling it.

```sh
gnome-extensions pack --force --out-dir /tmp vscode/client/gnome-shell/cursor-return
gnome-extensions install /tmp/cursor-return@intravenous.shell-extension.zip
gnome-extensions enable cursor-return@intravenous
```

Set `IV_WAYLAND_CURSOR_RETURN=1` when starting the patched VSCodium process
on native Wayland. The setting tells the graph editor that Chromium provides
correct click coordinates after pointer lock. Use `INTRAVENOUS_DEV_CODIUM` to
point the dev launcher at that executable. If the launcher opens a window in an
existing editor process, that process must already have the patched Electron and
the setting. The native helper discovers the companion through the session bus.

