# Front 7 event fan-in producer-home design

This note preserves a design that Front 7 superseded on 2026-10-22.

Before the Front 7 event-route work, the realtime event storage design allowed
semantic source 0 of a multi-source event connection to write into the fan-in
aggregate. Lowering could then merge the other source sequences into that same
storage. The design called this source the producer home. The storage cost model
compared this form with a separate fan-in aggregate.

Front 7 made raw event output storage belong to each concrete
`EventOutputPortId`. One output can now take part in more than one
`EventProducerGroupPlan`. It can also become a different connection source after
a graph transition. A fan-in aggregate therefore cannot replace or modify that
output's raw representation. If it did, the raw output would contain events from
other outputs, and another connection or graph transition could observe the
wrong stream.

The replacement design keeps one raw representation for each
`EventOutputPortId`. Each `EventProducerGroupPlan` has a separate fan-in
representation. Source conversion runs from the raw representation to the
group's `source_type`, then the merge writes the group representation. Common
connection conversion runs after the merge.

The current design is recorded in
[`graph_jit_direction.md`](../graph_jit_direction.md#tick-event-scc-capability-matrix)
and
[`sequential_port_storage_planning.md`](../sequential_port_storage_planning.md#sequential-port-storage-plans).
