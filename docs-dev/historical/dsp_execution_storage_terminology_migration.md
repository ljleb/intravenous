# DSP Execution And Storage Terminology Migration

> Historical terminology map retained for archaeology. Current terminology is defined in [`../dsp_execution_storage_glossary.md`](../dsp_execution_storage_glossary.md).

## Current source identifiers

The source/API rename now matches this vocabulary. Current code uses names such as
`Coverage`, `TockState`, `BackgroundEvaluationPlan`, and `BackgroundPortIndex`.
`Realtime*`/`Indexed*` are obsolete source and architectural spellings. They may
appear in historical material or in this replacement table only; remove them from
current code, comments, architectural prose, and examples.

## Preferred replacements for ambiguous umbrella language

Do not mechanically replace an old umbrella word with one new umbrella word. Choose
by meaning:

| Avoid as an architectural category | Use when that is the actual meaning |
| --- | --- |
| realtime graph | same-Tick scheduling graph; semantic dependency graph; or audio-thread execution |
| realtime dependency | same-Tick dependency |
| realtime storage | sequential-consumption storage; persistent temporal state; or audio-thread-safe storage, as applicable |
| realtime path | audio-thread path, when the thread constraint is the point |
| indexed graph | background evaluation DAG or semantic dependency graph |
| indexed input | random-access input |
| indexed output | Tock output, persisted output, background-requestable output, or another narrower description |
| indexed access | random access |
| indexed execution | background evaluation |
| indexed transaction / indexed batch | background evaluation transaction |
| indexed semantic version | semantic version |
| indexed page/version | persisted page / page version |
| indexed result | random-access result, only when the external access mode is the point |
| temporary page | transaction-local materialization; transaction-local page-backed materialization when page addressing is specifically required |

If none of the right-hand descriptions is relevant, remove the old qualifier rather
than inventing a replacement.
