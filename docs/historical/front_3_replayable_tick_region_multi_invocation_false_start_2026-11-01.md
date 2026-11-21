# Front 3 replayable Tick-to-Region multi-invocation false start

_Status: historical, non-normative._

_Provenance: removed from `docs/implementation_fronts_direction.md` on
2026-11-01 after review with the project author. The active resolution is in
[Front 3 source status](../implementation_fronts_direction.md#front-3-source-status-2026-10-26)._

## Preserved false start

One replayable-Tick-to-Region case appeared to remain as implementation friction. The
connection planner accepts that connection, as required by the compatibility table,
but `BackgroundEvaluationTransaction::Impl::run()` can place several disjoint ranges
in one `BackgroundEvaluationCallFrame`. The example used an intrinsically replayable
Tick/ephemeral source `A` connected to the only Region input of authored Tock node `B`.
It then gave `B` a Coverage output even though `B::tock_coverage()` expected exactly
one output Region. If one transaction selected `[0, 4)` and `[8, 12)`, replay
materialization gave `B` two independent input Region views. `B` could only call
`input<"in">().region()`, which requires exactly one view for that invocation.

`BackgroundEvaluationCallFrame::seal()` rejected this frame before generated
execution started. Removing that check would not have solved the example. The current
node-oriented generated root would replay `A` for both ranges and then invoke `B` once,
still with no valid choice for `B.in`.

The false start concluded that this was an unresolved conflict with the normative rule
that permits at most one authored Tock evaluation per implicated node for one
transaction. It considered invoking `B` once per Region, splitting the work into two
transactions, or rejecting replayable Tick/ephemeral outputs as Region-input sources.

## Resolution

The example's output declaration contradicted its callback contract. If `B` always
produces exactly one Region, `B.out` must use `OutputProduction::region`, not
`OutputProduction::coverage`. `BackgroundPropagationWorkspace::require_output()`
widens a partial demand on a non-Tick output to each complete intersected authored
Region before reverse propagation. A Region output therefore supplies one complete
required Region to this pass-through node.

If a node has a real Coverage output but needs one contiguous input Region, its reverse
propagation must map all selected output Regions to one `GlobalIndexRange`. If it maps
them to several disjoint input Regions, its input must use `InputAccess::coverage`.
`BackgroundEvaluationCallFrame::seal()` correctly rejects an authored Region input
that reaches evaluation with several Regions. No per-Region Tock invocation exception
is required.
