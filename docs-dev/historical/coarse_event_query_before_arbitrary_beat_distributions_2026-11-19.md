# Coarse event queries before arbitrary beat distributions

_Historical record, 2026-11-19. The owner wants to experiment with noisy
and arbitrary beat distributions. The proposal below assumed producers
and filters could often answer a query for coarse marks directly. The
[revised event observation design](../vue_client_implementation_stages.md#event-observation-for-arbitrary-beat-distributions-2026-11-19)
uses exact reads across supported sources._

From [Vue Client Implementation Stages](../vue_client_implementation_stages.md).

4. **Add Timeline nodes and event-grid snapping.** Establish a bounded
   coarse event query through producers and filters first. Observe the
   Timeline inputs project-wide and let the UI combine their events.
   Drive ruler marks and automation snapping from the same zoom-dependent
   list. See [Timeline nodes and project snapping](../project_playback_and_presentation_grids_direction.md#timeline-nodes-and-project-snapping).

### Coarse event observation design, 2026-11-19

The owner chose to design coarse event queries through producers and filters
before implementing the Timeline node and snapping UI.

**Implementation friction.** `observation_window_positions()` selects exact
indices for the Sample `waveform` operation. Reading events between those
indices requires a different request. The current
`resolve_observation_window_source()` resolves only Sample ports.
`Beats::tock_coverage()` generates every subdivision in the requested
`IndexCoverage`, and `TransactionEventStorage` grows with the events it
receives. Limiting the JSON result bounds transfer size. Generation and storage
can still grow with the number of events in view. Random access sinks also
need an input binding that exists outside a scheduled Tick consumer.
The module builder can declare an event input, and its resolved routes
provide the model for a passive Timeline input.

**Proposed query.** Add a background observation operation for an ordered
set of sample ranges. For each range, ask for the BeatEvent with the smallest
depth, breaking ties by distance to the range center, then by sample order.
The UI picks the range width from the zoom and aligns ranges to project time.
Return each selected event's exact timestamp and depth. At fan-in, the best
candidate from each route is enough to find the best candidate for the input.
The UI combines per-input results and applies its own display spacing.
Report empty ranges and unavailable results separately. Include the source
version and the ranges answered.

`Beats` can answer one range by testing its existing bar, beat, and
subdivision strides in depth order, using the existing sample rounding
rules. A constant time-shift node can shift the range and its center into
input time, then shift the exact result back. A node that discards events
by depth can narrow the allowed depths before querying its input. A node
that takes every third event needs to preserve input sequence positions.
It can use an index that supports that selection, or evaluate an exact
window within the work budget. Use the same bounded exact fallback for
nodes without an applicable query operation. Return unavailable when that
fallback cannot finish within budget.

Keep this as a read operation. The ordinary Tock path continues to produce
whole event sequences for its consumers. Cache coarse results by the query
and the input versions used to answer it. A projected candidate is valid
for that query and carries no claim that its range contains the whole
event stream.

**Dependency order.** Verify the event window query against exact reads,
first through `Beats` and then through a time shift and a depth filter.
Resolve and observe the passive Timeline input through the same graph
routes. Then wire the shared ruler and point-placement behavior.
