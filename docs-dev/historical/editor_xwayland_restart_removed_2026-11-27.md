# Retired XWayland Editor Restart

The owner removed automatic editor restarts and the dev launch backend
override on 2026-11-27. The text below records the previous behavior and
sequencing. See the current [development launch](../development_run.md) and
[native Wayland integration status](../vue_client_implementation_stages.md#native-wayland-cursor-return-status-2026-11-27).

## Development launch before removal

For cursor wrapping and cursor return on Wayland, start VSCodium with XWayland
if your desktop provides an X11 display. An existing VSCodium process keeps its
current display backend. Development windows honor the
`intravenous.autoRelaunchWithXWayland` user setting. If you enable it,
Intravenous can restart all open VSCodium windows with XWayland.
VSCodium restores windows according to its restore settings and original launch
arguments. If you added the development window to a running VSCodium instance,
you may need to open it again after the restart.

```sh
INTRAVENOUS_DEV_XWAYLAND=1 nix develop --command ./scripts/run_dev.sh projects/simple_sine
```

## Extension behavior before removal

On local Linux Wayland, Intravenous can offer automatic restart when a project
opens and VS Code has no explicit `--ozone-platform` flag. The notification
explains how XWayland improves cursor wrapping and cursor return after a drag.
It warns that enabling automatic restart restarts all open VS Code windows.

**Enable Auto-Restart** saves the enabled preference in User Settings and
requests a restart. **Keep Disabled** saves the disabled preference. Closing
the notification leaves the preference unset so it can appear the next time a
project opens. Use User Settings under Intravenous to change
`intravenous.autoRelaunchWithXWayland`.

When automatic restart is enabled, a short-lived launcher waits for the normal
VS Code quit action to finish. It starts the same editor with
`--ozone-platform=x11`. An explicit graphics backend takes precedence.
[Earlier Command Palette action](./vue_xwayland_restart_before_notification_2026-11-18.md).

The Intravenous output channel records each restart check and why it stops.
It also gives the path to `xwayland-restart.log`. The launcher uses that file
to record whether the editor exits and whether the replacement starts.

## Previous integration sequence

The integration depends on an Electron build with the correction, followed by
verification in an editor that uses it. The GNOME cursor move then needs to
enter the existing `PointerLockCursorReturn` path, and the corrected native
input path can use trusted mouse events directly. The release and regrab tests
must pass through the live graph editor before removing automatic XWayland
restart.
