# Front 6 event-output transition-suffix assumption

This note preserves a Front 6 implementation assumption that Front 7 superseded on
2026-10-22.

Front 6 represented retained event outputs through an indexed merged fan-in sequence.
The design note therefore stated that retained fan-in could accept event-output history
and latency directly, and that only a derived Sequential event input needed private
transition storage.

Front 7 moved canonical raw storage ownership to each concrete `EventOutputPortId`.
One raw sequence can now also serve a direct single-source consumer. When an event
output and an event input share that steady sequence, their inherited semantic states
can differ at cutover. Both states must use separate transition-only rings. The output
ring receives each new callback-local raw sequence after the producer executes. The
input ring receives the current steady delivery before the consumer executes.

The replacement is recorded in the Front 7 event-route resolution in
[`../implementation_fronts_direction.md`](../implementation_fronts_direction.md) and
the matching Front 7 entry in
[`../runtime_performance_roadmap.md`](../runtime_performance_roadmap.md).
