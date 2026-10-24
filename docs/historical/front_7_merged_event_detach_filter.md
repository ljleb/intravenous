# Front 7 merged event detach filtering

This note preserves a Front 7 implementation that was superseded on 2026-10-22.

The first per-source event detach implementation appended delayed events by reading
the producer group's sorted fan-in representation. `EventFeedbackPlan` carried a
`source_index_filter`, and the generated helper copied only events whose compiler-owned
source-index sidecar matched the detached source.

That representation does not support a numeric root-call cursor. A later SCC slice can
insert a newly authored event before events that were already present in the sorted
fan-in sequence. The sequence count grows, but the newly inserted event can be before
the stored cursor and is then not appended to the delayed stream.

The replacement keeps the source-index sidecar for the ordinary branches that are
selected from retained fan-in storage. A detached `EventSourcePlan` instead reads the
exact per-source representation already consumed by its producer-group merge. For a
cyclic producer, that callback-local or source-converted sequence starts at element
zero on each primitive invocation. `EventFeedbackPlan` therefore consumes the complete
sequence at each append scope. This keeps `detach()` at the removed dependency and does
not add a conversion stage.

The current design is recorded in
[`../implementation_fronts_direction.md`](../implementation_fronts_direction.md) and
[`../runtime_performance_roadmap.md`](../runtime_performance_roadmap.md).
