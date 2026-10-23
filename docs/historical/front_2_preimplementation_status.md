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
