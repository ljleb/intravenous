# Persisted Tick Recording Covers The Callback Block

_Historical status: non-normative. This file preserves the design text under which
a Tick/persisted output's recording item for one callback block covered exactly that
block, whatever the output's latency._

_Source, preserved 2026-11-10: [Coverage and Background Evaluation](../coverage_and_background_evaluation.md),
the paragraph on `tick_block()` dispositions in the section on recording._

_Reason for replacement: under the port time convention that sample and event
streams share since 2026-11-09
([event_output_future_window_2026-11-09.md](event_output_future_window_2026-11-09.md)),
a Tick output's latency marks its newest positions before the block as still open:
block `B` can revise sample values in `[B - latency, B)` and add events there. A
record of `[B, B + N)` therefore published values the next block could still revise
and never published events added inside the latency window, against "a tick/persisted
output retains finalized sequentially produced values"._

_Replacement (decided by the human co-author on 2026-11-10, after confirming that
Tick/persisted outputs may declare latency): block `B`'s record covers the positions
that block finalizes, `[B - latency, B + N - latency)`, with block `B`'s disposition.
See [Coverage and Background Evaluation](../coverage_and_background_evaluation.md)._

## Preserved text

For `tick_block()`, the disposition applies to the entire addressed callback block:
`written` publishes one authoritative overwrite for that block and `voided` publishes
one authoritative erase for that block. Scalar `tick()` has the same rule with a
one-sample block. History and latency remain legal mutation/storage facts, but do not
extend a recording item to `[block-history, block-end+latency)`. Recording start and
stop precision therefore follows Tick callback boundaries.
