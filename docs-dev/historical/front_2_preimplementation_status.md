# Front 2 pre-implementation status

Before the Front 2 implementation began on 2026-10-23,
[`batched_node_callbacks_direction.md`](../batched_node_callbacks_direction.md)
described its status as follows:

> Status: planned node API, trait-normalization, and GraphJit scheduling
> direction. The checked-in source does not yet provide the batch context types
> or compiler callback anchors described here.

That status was superseded when the public batch contexts, normalized helpers,
compiler anchors, ready-frontier grouping, and realtime/background lowering were
implemented. The direction document remains the normative contract; this file
retains the former implementation-status statement only.

Before the same implementation, the replayability sections in
[`batched_node_callbacks_direction.md`](../batched_node_callbacks_direction.md) and
[`coverage_and_background_evaluation.md`](../coverage_and_background_evaluation.md)
also described the former source restriction as follows:

> The checked-in implementation currently restricts the intrinsic replay trait to
> `tick()`-only nodes. The intended later direction is to allow normalized
> `do_tick_block()` to be a replay candidate when the callback-visible semantics
> permit it, for example when there is no realtime `State`, no relevant history or
> latency, and the deterministic/side-effect-free replay contract is satisfied.

Front 2 superseded that implementation-status statement. The normalized Tick family
now accepts `tick()`, `tick_block()`, or `tick_block_batch()` as the authored replay
form. The whole-graph planner still validates the resolved timing restrictions.

The former Front 2 entry in
[`implementation_fronts_direction.md`](../implementation_fronts_direction.md) also
carried this dependency note:

> Front 1 may later enrich the batch-compatibility key with solved
> size/pace/history/latency/ordering facts. That should not prevent this front from
> landing against the current realization identity first.

Front 2 superseded this note after Front 1. `same_callback_batch_class()` now compares
the callback-facing entries in `ResolvedPortRealization` before it assigns one class.

## Intermediate status before dormant-lane skip batching

Partway through the same implementation, on 2026-10-23, the status header of
[`batched_node_callbacks_direction.md`](../batched_node_callbacks_direction.md)
recorded this remaining gap:

> The traits layer and reflected compiler ABI provide normalized scalar and batch Tick
> and skip operations. GraphJit can invoke compatible active primitives through one
> `tick_block_batch()` call. Its activity lowering still selects `tick_block()` or
> `skip_block()` separately for each primitive. It does not yet partition one ready
> group into active and dormant lanes and invoke `skip_block_batch()` for the dormant
> subset. This is remaining Front 2 work; a root-level `skip_block` operation is not the
> solution because the generated Tick root owns each primitive's activity decision.

`try_emit_execution_batch()` in `graph_jit/lowering.cpp` later closed that gap
inside the generated Tick root. The current status is in the direction document's
header.

In the same intermediate state,
[`implementation_fronts_direction.md`](../implementation_fronts_direction.md) and
[`runtime_performance_roadmap.md`](../runtime_performance_roadmap.md) listed TTL
activity among the reasons a ready group stays scalar:

> It keeps a ready group scalar when TTL activity, primitive slicing, persisted
> capture, or producer-side storage operations would require extra control flow or
> longer transient live ranges.

> Realtime lowering uses native batches for uncomplicated groups and scalarizes
> groups with TTL activity, slicing, capture, or producer-side storage operations.
