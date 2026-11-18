# Automation point editing before placement mode

_Historical, non-normative. Preserved on 2026-11-18 when the curve editor gained
right-button insertion drags and Shift-controlled placement._

The earlier [Vue editing rules](../vue_presentation_refinement_direction.md#5-automation-editing-and-graph-authored-snapping)
specified:

> - **Left-click and drag a point** to move it. Grabbing an unselected point
>   leaves the current selection alone. Grabbing a selected point moves the
>   selected points together.
> - **Right-click empty space** adds a point. Point hit testing takes precedence.

The earlier [builtin automation guide](../../src/intravenous/builtin_packages/builtin/AUTOMATION.md)
described the original defaults:

> New points use Bézier interpolation with tension 1.
>
> - Click empty space to add a point at the nearest sample.
> - Click a point to select it. Drag it to move it between samples.
