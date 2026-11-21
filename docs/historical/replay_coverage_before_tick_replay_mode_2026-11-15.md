# Replay coverage before TickReplayMode

_Historical, non-normative. Preserved 2026-11-15 from the uncommitted working tree
on top of revision `d2e1d0ec`._ The owner replaced the boolean
`intrinsically_replayable` opt-in with one node-level `TickReplayMode`
(`disabled`, `input_union`, `full`). It sets intrinsic replay eligibility and
selects the compiler-generated output coverage. `input_union` keeps the earlier
generated union rule; `full` covers the whole circle. This also supersedes a
same-day agent reading that generated propagation must always give a
Sequential-only replayable node full coverage, and a pending per-output
proposal. Replacements:
[automation direction, section 4](../automation_and_suspendable_coverage_direction.md#4-absence-and-sequential-fallback),
the "Replayable node trait" section of
[Coverage, Random Access, And Background Evaluation](../coverage_and_background_evaluation.md),
and [Automation And Background Planning Migration](../automation_and_background_migration_plan.md).

The passages below are verbatim; links are interpreted from their original documents.

## Previous replayable node trait declaration

From `docs/coverage_and_background_evaluation.md`, "Replayable node trait":


A node opts into the intrinsic replay **type trait** by declaring
`static constexpr bool intrinsically_replayable = true` (default false).
`iv::details::intrinsically_replayable_v<Node>` is the validated compiler-facing
value; it is not a port field or an additional DSP callback. An opted-in node must define the
existing `tick()` but **not** its own native `tick_block()`, have no `State`, no
input/output history or latency after resolution, and zero internal latency. Random
Access inputs are permitted. Its tick computation must be deterministic and side-effect-free
under one immutable semantic/configuration version, declared inputs, and absolute
sample position; no mutable external or live-only resource may affect its result.
Validate instance-dependent properties such as internal latency after configuration.
Event replay, if enabled, must additionally preserve deterministic timestamp/order
and declared capacity; do not silently infer event replay from the sample rule.


## Previous absence clarification

From `docs/automation_and_suspendable_coverage_direction.md`, section 4, added
earlier on 2026-11-15:

**Absence stops at a Sequential input** (clarified by the owner, 2026-11-15).
A Tick node consumes its resolved Sequential input values and produces whatever
its output contract specifies; missing upstream coverage does not propagate
through it as absence. A value it computes from a neutral or manual fallback is
an actual output value, which a downstream fallback must not replace. Thus the
compiler-generated propagation of a replayable node whose inputs are all
Sequential, or that has no inputs, gives each Tick output every position. Its
inputs' coverage records where upstream contributions exist and must be read;
it is not the output's coverage. Output existence remains distinct from the
availability of the dependencies needed to replay it:

| Sequential dependency during replay | Replay behavior |
| --- | --- |
| Known uncovered | Use the input's declared fallback |
| Covered, value available for the selected version | Use the source value |
| Covered, value unavailable | Resolve the dependency, or leave the replay request unsatisfied |

This changes neither Region/Coverage inputs, nor authored output coverage, nor
the existing replayability requirements.


## Previous generated-propagation sentence

From `docs/coverage_and_background_evaluation.md`, section 8:

> A replayable Tick node with mechanically pointwise dependencies may continue
> using compiler-generated propagation. Its Tick outputs exist at every position,
> because its Sequential inputs resolve absent upstream positions; input coverage
> bounds only what replay must read (see
> [absence and Sequential fallback](./automation_and_suspendable_coverage_direction.md#4-absence-and-sequential-fallback)).

## Previous migration-plan checkpoint and proposal

From `docs/automation_and_background_migration_plan.md`:

#### Source checkpoint: generated replay coverage, 2026-11-15

Implemented in source; build and tests remain owner-run. `replay_ports_are_valid()`
requires authored propagation from any replayable node with a Region or Coverage
input, so `replay_forward_coverage()` serves only nodes whose inputs are all
Sequential, or that have none. It now publishes full-circle coverage for every
output. Changed positions remain the union of input changes (upstream coverage
transitions are reported as changes), or the full circle after a local change.
`replay_reverse_coverage()` already required only covered upstream positions.

A JIT regression (`SequentialOnlyTickReplayProducesOutputWhereNoUpstreamContributionExists`)
feeds `SingleRegionSampleSource`, covering [20, 24), through two replayable
Sequential Tick nodes into persisted windows that straddle that Region or lie
outside it; a replayable generator without inputs feeds another window. It
checks full output coverage, a cold-source requirement of exactly [20, 24),
and computed values at uncovered positions. Neutral is the only fallback until
the fallback feature lands, so the manual and changing-coverage cases are not
yet tested here.

Observation for step 3: a Tick event transformer with a Sequential event input
now has full-circle output coverage, with no events outside the point Region.
An interpreter whose reverse callback requires its whole input coverage, as
`LinearCurve::propagate_reverse_coverage()` does, would then require replay of
the whole circle. The planned "point source through an event transformer"
regression therefore needs either a transformer with Region/Coverage access and
authored propagation, which keeps the finite Region, or an interpreter that
discovers its neighboring points without requiring all input coverage.

#### Proposed, not accepted: declared output existence for replayable Tick, 2026-11-15

_Proposal from owner-relayed review; awaiting an explicit decision. It is not
yet normative._ Replayability (whether a Tick computation can be reproduced at a
requested index) and output coverage (whether it produces a value there) are
independent. A replayable Tick output could have one of three existence rules:

| Rule | Example | Output coverage |
| --- | --- | --- |
| Full (current generated default) | generator without inputs; gain whose Sequential inputs resolve by fallback | the whole circle |
| Declared gap-preserving | processor that keeps upstream gaps | union of the declared inputs' coverage, compiler-derived |
| Authored | value-dependent conditions | `propagate_forward_coverage()` |

The output buffer would not imply coverage; realtime and replay must agree on
which samples are contributions. This would resolve the event-transformer
observation above: a gap-preserving pointwise transformer keeps the finite point
Region, so the interpreter's reverse requirement stays finite.

Dependencies: realtime consumers of a gap-preserving output need its coverage
on the live path, so this depends on coverage-aware Sequential resolution.
Its declaration shape is unsettled, for example which inputs determine
existence when a node also has parameter inputs.
