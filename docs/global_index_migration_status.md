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
- `IndexCoverage` stores canonical ranges in an immutable path-copied AVL tree, so
  copied snapshots share coverage metadata and finite mutations replace only paths to
  ranges they touch; its ordered iterator uses bounded stack storage and does not
  allocate;
- Tick port windows, node Tick callback indices, generated-root callback indices,
  capture callback indices, feedback retention, event merging, reload-transfer
  indices, background transaction ingestion, and background storage realization use
  modular operations;
- event producer, fan-in, retained-chunk, and materialization contracts now name
  range-relative or signed-canonical ordering explicitly; no current contract relies
  on a context-free "absolute index order" over the circular domain;
- built-in per-sample fallback execution, latency rings, oscillator anchors,
  deterministic-noise grouping, debug display, and MIDI trigger offsets use explicit
  `GlobalIndex` advancement, distance, bit, or signed-display operations rather than
  recovering general integer operators;
- the shared fixed-width persisted-page representation has been removed from source;
- canonical persisted samples are immutable contiguous `PersistedSampleRegion`
  records, while canonical persisted events are explicit-range
  `PersistedEventChunk` records whose empty payload still authoritatively covers its
  range;
- each persisted output has a path-copied balanced range index selected through a
  path-copied compact-handle directory, and a candidate splits only records that its
  overwrite or erasure intersects;
- realtime persisted sample playback traverses intersecting regions and copies spans
  around ring-buffer wraps instead of looking up one page per sample and channel;
- persisted event playback traverses clipped chunks and binary-searches the first
  event in each selected chunk;
- final-design tests cover path-copy output indexing, exact overwrite/erase splitting,
  Tick random-access playback, wrapped Tick capture, capture-reserve exhaustion,
  authoritative empty event blocks, void erasure, queued transaction ingestion, and
  coherent generation-specific realtime publication; and
- earlier zero-clamping, saturation, begin/end ranges, and fractional-value naming
  have been preserved under `docs/historical/` and replaced normatively.

Work outside this completed feature boundary:

- the future node-facing `SampleRegion`/`SampleCoverage` port algebra remains a
  separate front from the store-internal `PersistedSampleRegion` representation; and
- compiler and test-suite verification of the coherent source, test, and normative
  documentation migration remains pending the co-author's requested build/test pass.

A static working-tree sweep finds no old page-store API, raw `SampleIndex`/
`EventTime`, `IndexRegion`, positional `Coverage`, or `.regions()` compatibility use
in source and tests. Normative documents now refer to fixed pages only when explicitly
rejecting or historically describing that superseded representation.

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

Accepted dependency change: the persisted-store replacement is now part of this
feature rather than a later prerequisite. The canonical store has one coherent
sample/event publication root, but samples and events have distinct record types and
range trees. Store-internal contiguous sample records deliberately do not introduce
the future node-facing `SampleCoverage` API early. Background transaction and
persisted-playback migration proceed against the new records rather than repairing the
shared page quotient.

## Friction discovered in persisted coverage metadata

Planned assumption: path-copying each changed output's range tree would make candidate
mutation and successor publication scale with the records actually changed.

Observed constraint at discovery time: each output state still owned a vector-backed
`IndexCoverage`.
Copying an output state and including/excluding one range copies or rebuilds every
historical coverage run for that output. A sparse recording with many written and
voided intervals therefore retains an O(total coverage runs) mutation cost even though
its data tree changes only O(log records) paths.

The constraint reaches beyond `PersistedDataStore`: background views and node-facing
coverage facades currently carry `IndexCoverage const*`, so replacing only the store's
metadata with a persistent interval tree would either require an O(n) flattening step
at every publication or introduce a second positional-coverage interface.

Accepted dependency change: keep the one `IndexCoverage` algebra and represent it as
an immutable persistent interval tree with a bounded non-allocating ordered iterator.
Copies share a root; include, exclude, union, intersection, and difference path-copy
or merge tree nodes; store snapshots, background callback views, and Tick callback
views continue to expose the same `IndexCoverage` base interface. `SampleCoverage`
remains the separate future data-bearing sample algebra. No store-only compatibility
coverage type or publication-time vector flattening is introduced.

Implementation status: the active working tree now contains that representation and
tests for immutable-copy semantics and signed-seam iteration. Compiler and test-suite
verification remains with the rest of the active migration.

The source migration is now intended to compile as one coherent feature; build and
test verification remains pending. No compatibility `SampleIndex`, `EventTime`,
`IndexRegion`, or `Coverage` aliases should be added to hide a diagnostic if that
verification finds an incomplete call site.
