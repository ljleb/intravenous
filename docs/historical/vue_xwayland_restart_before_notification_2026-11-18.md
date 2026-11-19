# XWayland restart before the notification

This section came from [Vue client implementation stages](../vue_client_implementation_stages.md).
The [project-open notification](../vue_client_implementation_stages.md#native-client-helper-2026-11-18)
replaced the Command Palette action on November 18, 2026.

> **Intravenous: Enable Automatic XWayland Restart** sets
> `intravenous.autoRelaunchWithXWayland` in User Settings. The setting defaults to
> off. On a local Linux Wayland session, the extension checks the desktop
> process at activation and when the setting is enabled. If the process has no
> explicit `--ozone-platform` flag, the extension requests the normal VS Code
> quit action. A short-lived launcher waits for that process to exit and starts
> the same editor with `--ozone-platform=x11`. An explicit graphics backend is
> preserved. The normal quit action applies to the entire editor instance.
