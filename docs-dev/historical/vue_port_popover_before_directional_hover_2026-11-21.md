# Port popover before directional hover

This text records the shared popover policy from the 2026-11-19 Vue
presentation design. The owner selected directional hover on 2026-11-21.
See the [current port popover design](../vue_presentation_refinement_direction.md#folded-inputs-extend-the-existing-lane-header).

> The owner chose cropped sockets and a shared continuation on 2026-11-19.
>
> Hover anywhere in the lane header to open one popover. In editable-height
> headers, each column's hidden rows start at the next position in that column,
> using the same knob size, row heights, and spacing as the visible rows. One
> column can start higher than the other when their visible rows differ. Each
> revealed port uses its own socket for cable endpoints. Scroll the additional
> rows when they exceed the available viewport height. Keep the continuation
> open while the pointer crosses between it and the header or while a knob is
> in use. During a cable drag, reveal the compatible destination column. Shift
> retains the separate per-port connection chooser.
