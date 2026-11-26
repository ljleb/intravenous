> Historical excerpts from [output_capabilities_and_background_demand.md](../../output_capabilities_and_background_demand.md) before the 2026-11-25 Tick coverage change. Follow that link for the replacement.

Tick callbacks produce ordinary values at executed positions, but computed
values and logical output coverage are separate. Acyclic `input_union` Tick
nodes derive coverage from their inputs; `full` nodes declare full coverage.
Sequential consumers apply their own fallback where that output is absent,
even when the upstream Tick callback computed a fallback value there. The
[recovered coverage contract](./tick_input_union_and_feedback_direction.md)
corrects the November 16 full-only wording, preserved
[historically](../../historical/tick_coverage_before_intent_recovery_2026-11-17/output_capabilities_and_background_demand.md).

---

effective full live coverage regardless of their declared replay mode. Their
declarations remain unchanged. No recursive coverage solver or independent
full-contribution proof is required. Explicit semantic SCC membership excludes
ordinary contextual background replay; it does not invalidate an input-union
declaration. The override is local to feedback SCCs.
