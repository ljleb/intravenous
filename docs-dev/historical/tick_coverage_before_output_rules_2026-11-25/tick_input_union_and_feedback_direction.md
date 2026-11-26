> Historical snapshot of [the Tick coverage contract](../../tick_input_union_and_feedback_direction.md) before the owner separated Tick coverage from replay on 2026-11-25. Follow that link for the replacement.

# Tick Input-Union Coverage And Feedback

_Status: recovered owner intent, 2026-11-17. Normative correction to the
November 16 documentation. Restored in source; owner verification is pending._

The owner supplied a recovery note after discovering that the November 16
migration incorrectly removed `TickReplayMode::input_union`. That removal was
not intended. The preceding documents and implementation notes are preserved
[historically](../tick_coverage_before_intent_recovery_2026-11-17/).
This contract takes precedence over statements that every executed Tick output
is covered or that input-union replay has been retired.

## Values and coverage are different

A Tick callback can compute a value at an executed position using a Sequential
input's neutral/manual fallback. That does not, by itself, make its output
covered. Downstream consumers use output coverage when deciding whether to
consume that value or apply their own fallback.

For acyclic Tick nodes:

- `TickReplayMode::input_union` declares intrinsic replay eligibility and derives
  generated output coverage from the union of input coverages.
- `TickReplayMode::full` declares intrinsic replay eligibility and generated full
  output coverage.
- `TickReplayMode::disabled` does not grant intrinsic background replay.
- Replay must be deterministic, side-effect-free, and pointwise. Replayable Tick
  nodes have no `State`, history, latency, Random Access input, or authored
  forward or reverse coverage callback. Intrinsic eligibility does not prove
  contextual replayability. Position shifts and authored propagation belong to
  Tock, not Tick.

For example, sparse automation feeding an acyclic input-union gain remains
absent in automation gaps. The gain may compute using its input fallback there,
but its downstream Sequential destination applies that destination's own
fallback. A gain supplied through an open default/manual input is not a new
full-circle connected source contribution. A separately connected full-coverage
producer would participate in the union normally.

## Feedback SCCs

Use the semantic dependency graph, including authored `.detach()` edges, to
identify feedback strongly connected components (SCCs). A feedback component
contains multiple mutually dependent nodes or an actual self-loop; singleton
acyclic components are not feedback.

Every Tick member of a feedback SCC has effective full coverage for live
execution, including members declared `input_union`. Keep the declaration
unchanged. Do not solve a recursive sparse-coverage fixed point, require an
independent full contribution, or reject an otherwise valid realtime feedback
graph merely because an SCC member declares input-union replay.

Feedback SCCs are not contextually replayable through ordinary position-addressed
background Tick replay, regardless of members' intrinsic replay declarations.
Check SCC membership explicitly. Published recordings can terminate a dependency
traversal without replaying their original feedback producer. An acyclic
processor may consume those selected recorded contents normally.

A future pure isolated background Sequential renderer is a separate execution
facility: it can copy and initialize an eligible Sequential subgraph for each
semantic Region and execute stateful feedback from that initial state. That
changes neither ordinary pointwise contextual replay eligibility nor the
feed-forward dependency DAG of background work. Overlapping Regions may have
different values at the same position because their instance histories begin
at different Region boundaries. The concrete Tock/input API remains undecided;
see [Project Playback](../../project_playback_and_presentation_grids_direction.md#future-isolated-background-sequential-rendering-distinct-from-playback).

## Unchanged output and storage contracts

Tick nodes declare Sequential outputs. Tock nodes declare Region or Coverage
outputs. Both retain `OutputRealization::{computed, buffered}`. Restoring
input-union semantics does not convert a Tick output declaration into a Tock
Coverage output, or restore whole-Region allocation.

Replayable nodes' own outputs remain computed. They may consume buffered data
produced by other nodes. Recording storage, logical coverage, published-valid
contents and selected demand windows remain distinct. Missing materialization
of known-covered data remains an underrun, never fallback.

## Implementation dependency order

1. Restore input-union declaration/serialization, propagation and downstream
   fallback for acyclic Tick nodes, with the explicit feedback-SCC override and
   replay exclusion. Keep position-addressed coverage propagation off the audio
   thread. Live Tick uses bounded presence lookups on the values it consumes.
2. Verify sparse acyclic chains, all-input-union feedback SCCs, recording
   boundaries, live delivery, replay and consumer fallback. The owner runs
   builds, tests and measurements.
3. Resume output gain using multiplication nodes and the existing control,
   specialization and migration paths. Remove unnecessary gain-specific LLVM
   and replay machinery instead of compensating for the regressed model.

The output-gain control/UI work remains unfinished. The reported 1,108-test
baseline verifies the previous source behavior; it does not establish that the
regressed full-only semantics match owner intent.
