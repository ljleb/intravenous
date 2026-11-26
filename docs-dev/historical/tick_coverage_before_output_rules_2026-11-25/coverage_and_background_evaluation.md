> Historical excerpts from [coverage_and_background_evaluation.md](../../coverage_and_background_evaluation.md) before the 2026-11-25 Tick coverage change. Follow that link for the replacement.

Intrinsic replay eligibility remains separate from port output shape and
realization. Replay is permitted only for deterministic, side-effect-free Tick
computations with no `State`, Sequential history or latency, Random Access
input, or authored forward or reverse coverage callback, and only when
contextual dependencies can be obtained from the same immutable snapshot.
`TickReplayMode::{disabled,input_union,full}` remains the intended declaration.
Outside feedback SCCs, input-union output coverage follows input coverage;
full output coverage spans the whole circle. Computing a fallback value does
not make an absent input-union output covered. Feedback SCC members instead
have effective full live coverage and are explicitly excluded from ordinary
contextual replay, without changing their declarations or solving recursive
coverage. See the [recovered contract](./tick_input_union_and_feedback_direction.md).
The erroneous November 16 full-only wording is preserved
[historically](../../historical/tick_coverage_before_intent_recovery_2026-11-17/coverage_and_background_evaluation.md).

---

`P` with Tick `ctx.index = I`. Their computed values and `input_union` coverage can
differ although the same Tick implementation is replayable. Preserve the distinction
between transport-selected live coverage and position-addressed background coverage,
without weakening intrinsic/contextual replayability. See
[Project Playback And Presentation Grids](../../project_playback_and_presentation_grids_direction.md#background-selection-coverage-and-pause).
