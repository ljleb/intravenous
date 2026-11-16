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

Each point stores a unique ID, a position in seconds, a Sample value, an outgoing
segment policy, and tension. Times are signed coordinates on the cyclic timeline.
The source rounds seconds to sample indices and rejects indices outside
`[-2^63, 2^63)`. Equal sample indices use ascending persistent point IDs.

A point's outgoing segment continues toward the next point, including across
the cyclic seam. `empty` produces no coverage. `hold` produces the point's value.
`bezier` interpolates toward the next point's incoming value. Segments include
their opening index and exclude their closing index. Zero points produce no
coverage. One empty point also produces no coverage. One hold or Bézier point
produces a full-circle constant.

Coincident points define jumps. The first point supplies the incoming endpoint.
The last point supplies the outgoing value and policy. Intermediate zero-length
segments contain no sample positions.

Bézier tension ranges from 0 to 1. Tension 0 gives exact linear interpolation.
Tension 1 gives spacing-aware, clamped automatic interpolation. New points use
Bézier interpolation with tension 1. The interpreter uses weighted harmonic
PCHIP derivatives. Derivatives flatten at extrema, jumps, and boundaries beside
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

The Vue presentation creates an empty point list for a new curve. Double-click
empty space to add a point. Drag a point to move it. Double-click a point to edit
its outgoing segment and tension. Right-click a point to remove it. Point drags
send coalesced `MovePoint` messages and form one undo operation per gesture.
`AddPoint`, `RemovePoint`, `MovePoint`, and `SetSegment` replace the module's
construction arguments. Project persistence and history retain their accepted
values. The editor displays those construction values, including real gaps and
the same automatic interpolation convention as the interpreter.

The former guide is preserved in
[historical documentation](../../../../docs/historical/automation_builtin_before_sparse_segments_2026-11-15.md).
