# Point cursor return before window destinations

From the native client helper section of
`docs-dev/vue_client_implementation_stages.md` on 2026-11-25. See the
[current native client helper notes](../vue_client_implementation_stages.md#native-client-helper-2026-11-18).

> The application setting `intravenous.warpCursorAfterPointDrag` enables the
> X11 prototype. The editor sends a short-lived request after pointer lock
> ends. The helper checks the restored pointer position before moving it.
> Requests expire and do not cross helper restarts. Native Wayland windows
> are outside the prototype.

For point drags, the helper compared the X11 pointer with the press position
in two candidate coordinate scales. The owner observed failure during a
normal release when the pointer had moved seven pixels horizontally and ten
vertically by the time X11 reported its position. The owner chose to send
the final point to the focused X11 editor window without that comparison.
