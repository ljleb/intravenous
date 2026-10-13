# Circular Global Index Migration Status

_Status: active implementation coordination, not normative design. The destination is
defined by [Circular Global Timeline Indexing](./global_indexing.md)._

## Active feature

Migrate every timeline-facing source/runtime/JIT interface from raw linear
`SampleIndex`/`EventTime` arithmetic to `GlobalIndex`, `GlobalIndexRange`, and
`IndexCoverage` without compatibility aliases.

Completed internal milestones:

- the strong `GlobalIndex` and begin-plus-extent `GlobalIndexRange` primitives exist;
- signed-canonical and range-relative ordering are explicit types;
- the old positional `Coverage` type has been replaced at its definition by modular
  `IndexCoverage` set algebra;
- Tick port windows, node Tick callback indices, generated-root callback indices,
  capture callback indices, feedback retention, event merging, and reload-transfer
  indices are being migrated to modular operations; and
- earlier zero-clamping, saturation, begin/end ranges, and fractional-value naming
  have been preserved under `docs/historical/` and replaced normatively.

Incomplete layers:

- background storage realization, background transaction ingestion, Tick persisted
  playback, and persisted page validation still contain linear begin/end and raw page
  arithmetic;
- tests still describe the displaced raw-integer and begin/end APIs; and
- the active migration intentionally has compiler errors until those layers and tests
  are converted.

## Friction discovered at persisted storage

Planned assumption: the strong-index migration could pass through the existing
persisted store before the sample Region/SampleCoverage front replaced sample pages.

Observed constraint: `PersistedPageStore` gives samples and events the same
`uint64 index / page_width` directory and page key. That calculation is neither a
modular range operation nor a signed-canonical chunk index. Repairing it for samples
would deepen a representation already selected for deletion. Repairing it for events
requires settling how finite event chunks are cut at the signed-order seam; a raw
fixed-width quotient cannot tile all 2^64 positions without a seam rule.

Complexity introduced by continuing unchanged: duplicated signed floor-division and
seam-splitting logic would spread through transaction ingestion, playback, storage
realization, page validation, and tests, only to be deleted from the sample half in the
next front.

Recommended dependency change: complete the circular-index migration through every
non-page execution layer, then replace persisted sample pages directly with
`SampleRegion`/`SampleCoverage`. At the same boundary, separate event-chunk indexing
from sample storage and define event chunks by explicit `GlobalIndexRange` plus stable
checkpoint/directory entries. Resume background transaction and persisted-playback
migration against those target representations rather than repairing the shared page
quotient.

The repository is not expected to compile while this feature remains active. No
compatibility `SampleIndex`, `EventTime`, `IndexRegion`, or `Coverage` aliases should
be added to hide the incomplete migration.

