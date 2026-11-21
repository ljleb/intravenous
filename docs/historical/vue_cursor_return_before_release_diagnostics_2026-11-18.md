# Cursor return before release diagnostics

_Historical, non-normative. Preserved on 2026-11-18 when cursor return gained
diagnostics for each failed request._

Source and replacement are in
[Vue client implementation stages](../vue_client_implementation_stages.md#native-client-helper-2026-11-18).

> The X11 helper logs the cursor position it observes and checks the result
> of traced warps.
> The helper also reports the first failed cursor-window check in the output
> channel. On Wayland sessions, this message explains the XWayland launch mode.
