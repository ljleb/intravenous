> Historical excerpts from [automation_and_suspendable_coverage_direction.md](../../automation_and_suspendable_coverage_direction.md) before the 2026-11-25 Tick coverage change. Follow that link for the replacement.

Outside feedback SCCs, an `input_union` declaration derives output coverage
from input coverage, while `full` declares full coverage. The callback's use
of fallback does not fill output gaps. A downstream Sequential input applies
its own fallback in those gaps. A buffered recording has separately published
historical coverage and valid contents. Contextual replay still requires the
selected dependencies to be available or reproducible.

The [2026-11-17 correction](./tick_input_union_and_feedback_direction.md)
restores that distinction. Only the recursive live-coverage proof is retired:
Tick members of feedback SCCs have effective full live coverage and cannot be
contextually replayed through ordinary background Tick replay. The declarations
remain unchanged. Earlier full-only wording is preserved
[historically](../../historical/tick_coverage_before_intent_recovery_2026-11-17/automation_and_suspendable_coverage_direction.md).

---

Tick nodes in a feedback SCC have effective full live coverage, including
members declared `input_union`. No independent full contribution or recursive
coverage fixed point is required. Preserve each declaration. Explicit semantic
SCC membership excludes ordinary background Tick replay even when every member
is intrinsically replayable. An initialized `.detach()` remains part of realtime
scheduling. Published recording boundaries can supply historical data without
replaying their feedback producer.

This override is local to feedback SCCs. Acyclic input-union nodes retain sparse
coverage.
