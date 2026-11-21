# Automation Point Clamping Before Output Bounds

_Historical. Preserved 2026-11-18 after the owner changed the bounds to clamp
each output sample after interpolation._

See the [cyclic automation direction](../automation_and_suspendable_coverage_direction.md),
the [Vue presentation direction](../vue_presentation_refinement_direction.md),
and the [builtin guide](../../src/intravenous/builtin_packages/builtin/AUTOMATION.md).

## Cyclic automation direction

`AutomationCurve` exposes optional `min` and `max` Sample Coverage inputs.
`PointSource` emits the authored `CurvePointEvent` values. `Curve` reads the
bounds at the samples of the points it needs for interpolation. It clips the
endpoints and tangent neighbors before it calculates each segment. Changes
to a bound between point samples do not change that segment.

An uncovered side supplies no limit. At each point, overlapping Regions supply
the highest lower bound and lowest upper bound. If these values cross, the
point evaluates to their midpoint. Nonfinite inputs supply no limit. The
point order, outgoing segment policy, tension, and gaps stay the same.

The editor keeps paired manual knobs in order. The bound inputs affect the
computed curve. Stored point values remain available for editing, and a drag
or a new point respects any enabled, unconnected manual bound.
[Previous constructor bounds](./automation_constructor_bounds_before_coverage_inputs_2026-11-18.md).

## Vue presentation direction

When signals supply bounds, observe the public `min` and `max` inputs over
the visible window. Draw their sampled values in the lane. Use the lowest
visible effective lower bound and highest visible effective upper bound to
limit panning. The editor uses representative samples for its preview. The
sampler uses exact values at point samples.

### Automation signal inputs and optional transforms

`builtin.automation.curve` exposes `min` and `max` as ordinary Sample
Coverage inputs. The sampler applies them at point samples before it
interpolates. At each point, the highest lower bound and lowest upper bound
apply. If they cross, the point evaluates to their midpoint.

### Explicit value bounds and lane zoom categories

The lane header shows the connectable `min` and `max` knobs. Each knob has an
enable button. An enabled minimum limits the maximum knob from below, and an
enabled maximum limits the minimum knob from above. If enabling a knob would
cross the opposite side, move the knob being enabled to the opposite value.
The project stores the manual input values and their enabled states.

The Scale panel controls display and snapping. With two distinct finite
limits, map the value height linearly between them. With one or no limit, use
the curved mapping. If the limits are equal, leave room around the value so
the point remains editable.

Shift and middle-drag pans the value scale when the view has room to move.
The gesture also pans the timeline horizontally. The cursor wraps inside
the visible lane content to the right of the header. With neither bound,
move the center of the arctangent scale. With one bound, keep that edge
fixed. Shift and a double middle click restores the default mapping.

Changing bounds preserves existing authored `Sample` values. Dragged and new
points respect enabled, unconnected manual bounds. The first movement of a
point brings it inside those bounds. Point edits do not rescale the axis to
fit the points.
[Earlier point drag behavior](./automation_point_drag_before_bound_limits_2026-11-18.md)
[Previous constructor bounds](./automation_constructor_bounds_before_coverage_inputs_2026-11-18.md).

## Builtin guide

Use the `min` and `max` inputs in the lane header to set or connect bounds.
The regular input knobs set manual values and enable each bound separately.
If enabling one knob would cross the other enabled knob, the knob being
enabled moves to the opposite value.

The sampler reads each bound at the control point sample before it
interpolates. An uncovered side supplies no limit. The highest lower bound
and lowest upper bound apply when Regions overlap. If the limits cross, the
point evaluates to their midpoint. Changes to bounds preserve stored point
values. Dragged and new points respect enabled, unconnected manual bounds.

The Scale panel selects the display and snapping rules. Connected bounds
appear as sampled traces. The editor uses their visible values to limit
panning.
