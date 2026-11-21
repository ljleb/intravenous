# Automation before cyclic semantics

Historical semantics and implementation status, preserved 2026-11-14 from revision
`fb6079f1` plus the uncommitted working tree. The owner corrected the automation
semantics: the global timeline is cyclic, the duration construction argument is
removed, and the curve interpolates the last stored point with the first across the
global wrap. The curve module sorts the points it receives; the owner accepted that
the point source may keep rejecting unsorted input. The replacements are
[Automation points and curves](../../src/intravenous/builtin_packages/builtin/AUTOMATION.md)
and the "Cyclic automation semantics" section of
[Vue Client Implementation Stages](../vue_client_implementation_stages.md).

The following three passages are verbatim. Links in the guide are interpreted from
its original location, `src/intravenous/builtin_packages/builtin/AUTOMATION.md`.

## Previous builtin package guide

# Automation points and curves

The default builtin IV package provides a sparse Tock point source, a replayable
Tick linear interpolator, and an editable automation-curve module. They require
no additional package or native dependency. Their registered IDs are
`builtin.automation.point_source`, `builtin.automation.linear_curve`, and
`builtin.automation.curve`.

```cpp
#include <intravenous/basic_nodes/automation.h>

void build(iv::GraphBuilder& graph) {
    auto curve = graph.node<"builtin.automation.curve">(
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

The two leaves can also be used separately. `builtin.automation.point_source` takes
an `ImmutableArray<Point>` created by `graph.immutable_array<Point>()` and the
duration. Its events can pass through other event-processing nodes before
reaching `builtin.automation.linear_curve`.

The source stores events rather than a sampled buffer for the clip. The curve
reader searches for neighboring points and samples the requested positions.
An empty point list or a position before the first point produces zero. The
last value is held through the last sample position in the source Region.
The rounded sample position of `duration` is included. Equal-position events
are retained; the last event there supplies the value at that position.

The Vue client includes a specialized presentation for a direct
`builtin.automation.curve` instance. After rebuilding the application and
restarting its server, choose that definition in Add Node. For an empty
five-second curve, use `{}, 5.0` as its construction arguments. A populated
illustrative configuration source is:

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
The owner verified the editor's automated tests before builtin promotion. This
packaging and ID change awaits a rebuild and verification.
The previous optional-package guide and IDs are preserved in
[historical documentation](../../../../docs/historical/automation_before_builtin_promotion_2026-11-14.md).

## Previous bounded interpolation policy

From `docs/vue_client_implementation_stages.md`, the builtin automation definitions
under Stage 3:


The first interpolation policy is linear. Events at the same position retain
stable order. The last event at a position supplies that position's held value;
interpolation approaching that position uses the first event there. An empty
point list, a position before the first point, or a position outside the bound
Region produces zero. After the last point, its value is held to the Region's
end. Point-source coverage includes the rounded sample position of `duration`.
These are the initial node's policies, not universal rules for Sample events.

## Previous cyclic-semantics status

From `docs/vue_client_implementation_stages.md`, before the implementation was
completed:

### Cyclic automation semantics, 2026-11-14

Accepted owner correction; implementation is pending. The global timeline is
cyclic. Automation has no semantic beginning, end, or duration. The point list
alone defines the curve: interpolate between successive points in cyclic order,
including the segment from the last stored point to the first across the global
wrap. This uses the actual `GlobalIndex` cycle, not a repeating clip whose period
is the distance between the earliest and latest displayed points. One point
therefore defines a constant curve. An empty list keeps the existing zero-value
behavior.

Remove the explicit duration argument and the restrictions that pin points to
`[0, duration]`. A finite event-storage Region is an implementation representation
of the stored point set; it must not cut off the interpreted curve outside that
Region. Cyclic interpolation uses modular index distances. The current
`PointSource::range()`, `LinearCurve::tick_block()`, and frontend curve model still
implement bounded semantics and need to change together.

The specialized presentation supplies the initial construction data and manages
ordinary edits through typed semantic messages. Users should not need to type
constructor expressions for normal creation or editing. Raw configuration remains
available for advanced use. Displayed time windows do not bound the curve.

Implementation in progress: add an explicit cyclic mode to indexed event-neighbor
lookup, retain one sparse point-storage Region, and publish full positional
coverage for the replayable interpreter. Carry the accepted sample rate in the
graph-scope read model so the frontend uses the same global cycle. Remove
duration from construction/replacement and provide empty-list creation data
through the presentation registry. Update the renderer, light boundary tests,
