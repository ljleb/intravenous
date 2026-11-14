# Automation points and curves

This optional IV package provides a sparse Tock point source and a replayable
Tick linear interpolator. Load this package alongside a project package that
uses its registered definitions. It is not added to default builtin discovery.

```cpp
#include <intravenous/basic_nodes/automation.h>

void build(iv::GraphBuilder& graph) {
    auto curve = graph.node<"iv.automation.curve">(
        std::vector<iv::automation::Point>{
            {1, 0.0, 0.0f},
            {2, 1.0, 1.0f},
            {3, 5.0, 0.25f},
        },
        5.0);
    graph.outputs(curve); // Sample output: linearly interpreted values.
    graph.event_outputs(curve.event_port()); // Event output: original points.
}
IV_MODULE("my.automation", build);
```

Each point has an instance-local ID, a position in seconds, and a Sample value.
IDs must be unique. Times must be finite and lie between zero and the supplied
duration, inclusive. Values must be finite. The module sorts points by time and
preserves their order when times are equal.

`AddPoint`, `RemovePoint`, and `MovePoint` are independent semantic event types
in `iv::automation`. They replace the module's construction arguments through
`ModuleMessageContext::replace()`. The backend's existing replacement history
and project persistence retain the accepted point list.

The two leaves can also be used separately. `iv.automation.point_source` takes
an `ImmutableArray<Point>` created by `graph.immutable_array<Point>()` and the
duration. Its events can pass through other event-processing nodes before
reaching `iv.automation.linear_curve`.

The source stores events rather than a sampled buffer for the clip. The curve
reader searches for neighboring points and samples the requested positions.
An empty point list or a position before the first point produces zero. The
last value is held through the last sample position in the source Region.
The rounded sample position of `duration` is included. Equal-position events
are retained; the last event there supplies the value at that position.

The specialized Vue curve editor and typed frontend bindings are pending.
