# Builtin automation before sparse segment policies

Historical, non-normative. Preserved on 2026-11-15 from
`src/intravenous/builtin_packages/builtin/AUTOMATION.md` before the sparse
computed automation implementation. The predecessor described full-circle
linear interpolation and an earlier Tick implementation. The replacement is
[the builtin automation guide](../../src/intravenous/builtin_packages/builtin/AUTOMATION.md).

---

# Automation points and curves

The default builtin IV package provides a sparse Tock point source, a replayable
Tick linear interpolator, and an editable automation-curve module. They require
no additional package or native dependency. Their registered IDs are
`iv.builtin.automation.point_source`, `iv.builtin.automation.linear_curve`, and
`iv.builtin.automation.curve`.

```cpp
#include <intravenous/basic_nodes/automation.h>

void build(iv::GraphBuilder& graph) {
    auto curve = graph.node<"iv.builtin.automation.curve">(
        std::vector<iv::automation::Point>{
            {1, 0.0, 0.0f},
            {2, 1.0, 1.0f},
            {3, 5.0, 0.25f},
        });
    graph.outputs(curve); // Sample output: linearly interpreted values.
    graph.event_outputs(curve.event_port()); // Event output: original points.
}
IV_MODULE("my.automation", build);
```

Each point has an instance-local ID, a position in seconds, and a Sample value.
IDs must be unique, and times and values must be finite. Times are signed
coordinates on the cyclic global timeline; the list has no duration. The module
sorts points by time and preserves their order when times are equal. The point
source rejects a point whose rounded sample position lies outside the signed
timeline view, `[-2^63, 2^63)` samples.

`AddPoint`, `RemovePoint`, and `MovePoint` are independent semantic event types
in `iv::automation`. They replace the module's construction arguments through
`ModuleMessageContext::replace()`. The backend's existing replacement history
and project persistence retain the accepted point list.

The two leaves can also be used separately. `iv.builtin.automation.point_source` takes
an `ImmutableArray<Point>` created by `graph.immutable_array<Point>()`. Unlike the
module, it does not sort: it rejects unsorted or non-finite points. Its events can
pass through other event-processing nodes before reaching
`iv.builtin.automation.linear_curve`.

Time in Intravenous is cyclic: the global timeline has 2^64 sample positions
and wraps. The curve interpolates linearly between successive points in cyclic
order, including from the last point to the first across the wrap. One point
therefore defines a constant curve; an empty point list produces zero.
Equal-position events are retained; the last event there supplies the value at
that position, and interpolation approaching it uses the first event there.

The source stores events rather than a sampled buffer. Its one finite Region
covers every point and excludes the largest cyclic gap between neighboring
points. That Region is storage only: the curve publishes coverage for the whole
cycle, searches for the neighboring points of each requested position, and
samples only the requested positions.

The Vue client includes a specialized presentation for a direct
`iv.builtin.automation.curve` instance. After rebuilding the application and
restarting its server, choose that definition in Add Node. The editor fills in
`{}`, an empty point list, as the construction arguments of a new curve. A
populated illustrative configuration source is:

```cpp
{{.id = 1, .seconds = 0, .value = 0},
 {.id = 2, .seconds = 1, .value = 1},
 {.id = 3, .seconds = 5, .value = 0.25}}
```

The only argument is the point list. This is C++ configuration-expression syntax,
not JSON. Double-click the curve to add a
point, drag a point to move it, and right-click a point to remove it. A drag sends
coalesced semantic messages and makes one undo entry. Accepted construction values
restore the editor after reload or undo; they are not output observations.
The owner verified the editor's automated tests before builtin promotion. The
packaging and ID change and the cyclic semantics await a rebuild and verification.
The previous optional-package guide and IDs are preserved in
[historical documentation](../../../../docs/historical/automation_before_builtin_promotion_2026-11-14.md);
the duration-bounded guide is preserved in
[historical documentation](../../../../docs/historical/automation_before_cyclic_semantics_2026-11-14.md).
