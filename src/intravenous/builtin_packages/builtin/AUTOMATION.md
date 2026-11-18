# Automation points and curves

The builtin package provides `iv.builtin.automation.curve`, an editable module.
Its leaves are `iv.builtin.automation.point_source` and
`iv.builtin.automation.interpreter`. Both leaves execute as Tock nodes. The
point source publishes buffered event data. The interpreter computes Sample
Coverage data only for requested windows.

```cpp
#include <intravenous/basic_nodes/automation.h>

void build(iv::GraphBuilder& graph) {
    using namespace iv;
    auto curve = graph.node<"iv.builtin.automation.curve">(
        std::vector<iv::automation::Point>{
            {1, 0.0, 0.0f, iv::CurveInterpolation::bezier, 1},
            {2, 1.0, 1.0f, iv::CurveInterpolation::hold, 1},
            {3, 5.0, 0.25f, iv::CurveInterpolation::empty, 1},
        });
    graph.outputs("value"_P = curve, "points"_F = curve.event_port());
}
IV_MODULE("my.automation", build);
```

Each point stores a unique ID, a time, a Sample value, an outgoing segment
policy, and tension. The editor writes the time in `sample_index` as a signed
decimal string. For example, at 48 kHz, `.sample_index = "48000"` means one second
after index zero in a project configuration expression. When `sample_index`
is empty or omitted, the source rounds `seconds` at the project's sample rate,
as in the C++ example above. The source accepts indices in `[-2^63, 2^63)`.
Equal sample indices use ascending persistent point IDs.

A point's outgoing segment continues toward the next point, including across
the cyclic seam. `empty` produces no coverage. `hold` produces the point's value.
`bezier` interpolates toward the next point's incoming value. Segments include
their opening index and exclude their closing index. Zero points produce no
coverage. One empty point also produces no coverage. One hold or Bézier point
produces a full-circle constant.

Coincident points define jumps. The first point supplies the incoming endpoint.
The last point supplies the outgoing value and policy. Intermediate zero-length
segments contain no sample positions. At the same sample, points follow their
saved `order`, then their ID. The default `order` is zero.

Bézier tension ranges from 0 to 1. Tension 0 gives exact linear interpolation.
Tension 1 gives spacing-aware, clamped automatic interpolation. The interpreter
uses weighted harmonic PCHIP derivatives. Derivatives flatten at extrema, jumps, and boundaries beside
hold or empty segments. Intermediate tensions blend the linear and automatic
curves. Values stay within the segment endpoint values.

`CurvePointEvent` carries the value, policy and tension. Editing IDs remain in
the source's construction data. The finite Region that stores these events does
not limit the curve's active intervals. The interpreter discovers its actual
input events before publishing coverage, so event transformations affect both
coverage and values. No conversions to unrelated event types are implicit. An empty point source
publishes a one-sample Region containing zero events. This keeps its empty group
explicit without adding curve coverage.

Sequential destinations resolve uncovered positions with their declared neutral
or manual fallback. Region and Coverage destinations retain the gaps. Pending
background data never count as a gap.

The Vue presentation creates an empty point list for a new curve. The clip ruler
shows seconds and sample positions. Zoom in to see individual sample marks.

- Right-press empty space to add a point at the nearest sample and start placing it.
- Drag a point to move it with its curve.
- Hold Shift while dragging to keep the point's original type and tension.
- Shift-click a point to remove it.
- Right-click a point to set its outgoing segment or choose Delete point.
- Drag the diamond beside a Bézier point left or right to adjust its tension.

A time detent holds the dragged point at a neighbor's sample while the pointer
moves. The first half of the detent puts the point before its neighbor. The
second half puts it after. Shift changes the placement mode at any time.

A new point in a gap connects to the nearer edge and leaves the other side
empty. The first point on a blank curve is empty. An existing endpoint stays
attached as it moves in a gap. It joins another curve when it crosses that
curve's edge. The old neighbor becomes the end of the first curve.
[Previous placement gestures](../../../../docs/historical/automation_point_drag_before_time_detents_2026-11-18.md#previous-builtin-automation-guide).

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

Point and tension drags each form one undo operation.
Bound drags also form one undo operation. `AddPoint`, `RemovePoint`, `MovePoint`,
`PlacePoint`, `SetSegment`, and `SetBound` replace the module's construction arguments.
`PlacePoint` updates the point, its neighboring segments, and the order of
points at the same sample together.
Project persistence and history retain their accepted values. All lane
heights display the same curve shape and gaps. Zoom to
`editable` height to display its editing controls.

See the [earlier seconds-only editor](../../../../docs/historical/automation_builtin_before_sample_editing_2026-11-18.md)
and the [guide before sparse segments](../../../../docs/historical/automation_builtin_before_sparse_segments_2026-11-15.md).
