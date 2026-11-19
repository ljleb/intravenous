# Combined zoom before fixed pickup

Preserved on 2026-11-19 from `docs/vue_client_implementation_stages.md`.
The owner chose independent drag axes and a fixed initial anchor for
Ctrl+Alt+middle drag, with or without Shift. See the [current behavior](../vue_client_implementation_stages.md#editor-managed-ruler-snapping-and-combined-zoom).

> Ctrl+Alt+wheel and Ctrl+Alt+middle drag scale both project axes about the
> pointer. A drag toward the right or bottom zooms out. Adding Shift at drag
> pickup selects lane zoom independently on each axis. Automation lanes supply
> vertical value zoom. Lanes without a zoom handler on an axis use the project
> timeline for that axis.

In the previous implementation, `combinedMiddleDragZoomPixels()` added X
and Y motion into one zoom amount. `applyCombinedMiddleZoom()` used the
moving pointer as the anchor for both axes.
