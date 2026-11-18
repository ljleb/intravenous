# Automation Constructor Bounds Before Coverage Inputs

_Historical. Preserved 2026-11-18 when the owner approved connectable bounds
and removed the requirement to migrate constructor bounds._

The current rules are in the [cyclic automation direction](../automation_and_suspendable_coverage_direction.md),
the [Vue presentation direction](../vue_presentation_refinement_direction.md),
and the [builtin automation guide](../../src/intravenous/builtin_packages/builtin/AUTOMATION.md).

## Cyclic automation direction

`AutomationCurve` stores independent minimum and maximum settings alongside
the authored point list. Each setting consists of a finite value and an enabled
flag. `PointSource` limits the value in each `CurvePointEvent` to the enabled
bounds before the interpreter computes the curve. The stored points retain the
original values. Their event positions, outgoing segment policy, tension, and
sparse coverage retain the same rules. When enabled bounds meet, all emitted
point values equal that bound. When a bound edit crosses the enabled opposite
side, `SetBound` limits the edited value and preserves the opposite setting.
`AddPoint` and `MovePoint` limit the edited point to the enabled bounds and
preserve every other point. Existing out-of-range values survive project
load, bound changes, and changes to segment type or tension.
[Earlier display-only bounds](./automation_display_only_bounds_2026-11-18.md)

The presentation uses the enabled bounds for its vertical axis. Point edits
never rescale it automatically. Both `nano` and `mini` show the curve and its
gaps read-only. Point and tension editing are available at `editable` zoom.
[Vue Presentation Refinement](../vue_presentation_refinement_direction.md#explicit-value-bounds-and-lane-zoom-categories)

## Vue presentation direction

When bounds are supplied by signals, show their observed value curves and use
the minimum of the visible lower bound and the maximum of the visible upper
bound to limit panning. This requires suitable node inputs and windowed
observation of the values they receive. The current automation module stores
its bounds in its construction arguments. Input window observation can read
concrete and public Coverage inputs.

### Automation signal inputs and optional transforms

_Future direction, recorded on 2026-11-18._

Expose `min`, `max`, `scale`, and `offset` as ordinary connectable inputs on
`iv.builtin.automation.curve`. Apply the observed bounds to point events
before interpolation so clamping preserves smooth segments. Apply scale and
then offset to sampled values so other curves can modulate both. In the absence
of a scale or offset contribution, use an identity transform. Preserve the
curve's sparse intervals and decide how overlapping control Regions combine
as part of the input design.

Allow an optional `NodeRef` constructor argument to supply a subgraph that
transforms the curve's authored point events before sampling. A second optional
`NodeRef` can supply a subgraph that processes sampled values. The presentation
can draw the original and transformed curves in the same lane. Implementation
must preserve the original curve and make both sampled signals observable.
The arguments must follow the
[builder ownership rules](./configuration_expression_language_direction.md#implemented-decisions-and-remaining-dependency).

_Implementation friction, 2026-11-18._ `build_structural_sample_routes` follows a
public Coverage input to its incoming connections. When a leaf reads the public
input and the input has no source, the planner can reject the graph. A curve
with four optional inputs needs to compile with no connections or manual values.
First allow structural routing to represent an uncovered public Coverage
input. Then wire the four inputs without adding a synthetic source for each
disconnected port. The bounds change must also preserve saved
`AutomationCurve` values and respect the existing rules for editing points.

_Implementation update, 2026-11-18._ An unconnected public Coverage input can
now route as empty, including when an internal leaf consumes the port. The
compiler retains the public route for window queries. Wiring the four
automation inputs and migrating the saved bounds remain separate work.

### Explicit value bounds and lane zoom categories

The Scale panel exposes separate **Minimum** and **Maximum** knobs for the
bounds saved by `AutomationCurve`. Each knob has an enable button in the same
position as the output knob's polarity button. Bound knobs have no connection
ports. An enabled minimum limits the maximum knob from below. An enabled
maximum limits the minimum knob from above. If a disabled bound's stored value
crosses the enabled opposite side, enabling it moves that bound to the opposite
side's value. The opposite bound keeps its value. The knobs preview locally
while dragged and commit on release.

The drawn curve uses the bounded values emitted by `PointSource`. With two
distinct enabled bounds, map the value height linearly between them. With one
or no enabled bounds, use the curved mapping from the one-sided or unbounded
knob. If both bounds are equal, show the constant value with room around it.
Shift and middle-drag pans the value scale when at most one bound is enabled.
The same gesture pans the timeline horizontally. The cursor wraps within the
visible lane content to the right of the lane header.
With neither bound, shift the center of the arctangent scale. With one bound,
keep that edge fixed and change the distance from it to the midpoint. The
graph layout saves the lane's pan. Shift and a double middle click restores
its default mapping.
Changing bounds preserves existing authored `Sample` values. The first actual
movement of a point drag brings the dragged point inside the enabled bounds,
including a horizontal drag. Add new points inside the enabled bounds. Point
edits never rescale the vertical axis to fit the points. Use the existing
lane-level vertical zoom. Save bound settings in the project with the automation
module so they survive presentation remount and project reload.
[Earlier point drag behavior](./automation_point_drag_before_bound_limits_2026-11-18.md)
[Earlier display-only bounds](./automation_display_only_bounds_2026-11-18.md)

## Builtin automation guide

Open `Scale` to set the Minimum and Maximum knobs. The button beside each
knob enables that bound. Each enabled bound limits the other knob. If you
enable a bound after the opposite knob has passed it, the bound you just
enabled moves to the opposite knob's value.

The project saves the bounds with the automation curve. The point source
applies the bounds to point events before interpolation. Each stored point
keeps its original value when you change a bound. Dragging a point brings it
inside the enabled bounds, even if you only move it in time. A new point
starts inside the bounds. Two distinct enabled bounds give a linear scale.
One enabled bound or no enabled bounds give the same curved scale as the
knobs. Choose Value snapping in the same panel to set the point guides.
Projects with older display-only bounds start with both audio bounds disabled.
