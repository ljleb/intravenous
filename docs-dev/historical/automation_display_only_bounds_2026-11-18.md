# Display-only automation bounds

Historical excerpts preserved on 2026-11-18. The owner moved the minimum
and maximum into the automation module and applied them to its point events.
[Current value bounds](../vue_presentation_refinement_direction.md#explicit-value-bounds-and-lane-zoom-categories)

From `docs-dev/vue_presentation_refinement_direction.md` at revision `7a1e018f`:

> Let the automation curve presentation specify an optional **minimum** and
> optional **maximum** value, independently. These are stable presentation/value
> mapping controls, not a curve-fitting algorithm driven by the highest or
> lowest point. When **both** finite bounds are supplied, the clip maps value
> height **linearly** between them. With **at most one** bound, use a monotone,
> invertible **curved** value mapping consistent with the familiar unbounded or
> one-sided knob behavior. Its visible orientation and range should not jump
> when a point is dragged, created, deleted, or given a new value; editing points
> must never automatically rescale the vertical domain to fit them. Do not add
> a second vertical zoom gesture inside the automation presentation: use the
> existing lane-level vertical zoom only. Keep actual authored `Sample` values
> independent of the display transform, even when outside the visible bounds.
> The bounds and their ownership must survive presentation remount and project
> reload without altering the DSP curve's interpolation or sparse coverage.

From `docs-dev/automation_and_suspendable_coverage_direction.md` immediately before
the backend-bounds change, starting from revision `7a1e018f`:

> The curve presentation additionally has independently optional value bounds:
> with two bounds use a linear value axis; with one or none use the same kind
> of curved value mapping as an unbounded knob. Editing points never rescales
> that axis automatically. In lane `nano` zoom show only exact covered/empty
> time intervals; in `mini` show the curve read-only; expose point/tension
> editing only at `editable` zoom.

From the working copy of the builtin guide immediately before the backend-bounds change:

> The `Scale` control sets a minimum and maximum independently. Leave either
> field empty to keep that side unbounded. Two bounds give a linear scale.
> One bound or no bounds give the same curved scale as the knobs.
> This scale maps the point values and the curve between them to the lane.
> Choose Value snapping in the same panel to set the guides points snap to.
> The graph layout saves the bounds separately from the point values.

[Current builtin guide](../../src/intravenous/builtin_packages/builtin/AUTOMATION.md)
