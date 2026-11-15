# Automation before builtin promotion

Historical implementation status, preserved 2026-11-14. The owner requested that
these dependency-free definitions ship in the default builtin package and use
`iv.builtin` IDs. The replacement guide is
[Automation points and curves](../../src/intravenous/builtin_packages/builtin/AUTOMATION.md).

The following guide was `src/intravenous/builtin_packages/automation/README.md`
before that package was removed. Its links are interpreted from that original
location.

## Previous package guide

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

The Vue client includes a specialized presentation for a direct
`iv.automation.curve` instance. Load this optional package in the project's package
list, then create an instance of that definition. For its two construction
arguments, an illustrative configuration source is:

```cpp
{{.id = 1, .seconds = 0, .value = 0},
 {.id = 2, .seconds = 1, .value = 1},
 {.id = 3, .seconds = 5, .value = 0.25}}, 5.0
```

The first argument is the point list; the second is duration in seconds. This is
C++ configuration-expression syntax, not JSON. Double-click the curve to add a
point, drag a point to move it, and right-click a point to remove it. A drag sends
coalesced semantic messages and makes one undo entry. Accepted construction values
restore the editor after reload or undo; they are not output observations.
The editor and its new backend support await owner build, test, and UI checks.
The earlier pending status is preserved in
[historical documentation](../../../../docs/historical/stage_3_before_automation_presentation_2026-11-14.md).

## Superseded stage-status passages

From `docs/vue_client_implementation_stages.md`:

The new optional `builtin_packages/automation` package registers:

- The automation IV package remains optional. To exercise the specialized editor,
  load that package and create a direct `iv.automation.curve` instance; see its
  [package guide](../src/intravenous/builtin_packages/automation/README.md).
