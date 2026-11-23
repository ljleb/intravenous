# Superseded Fixed-Width Persisted-Page Design

_Status: historical, non-normative. Preserved on 2026-10-13 when canonical persisted
samples changed to contiguous range records and canonical persisted events changed to
explicit-range chunks. The replacement is documented in
[`coverage_and_background_evaluation.md`](../coverage_and_background_evaluation.md)
and [`dsp_execution_storage_glossary.md`](../dsp_execution_storage_glossary.md)._

The text below is the displaced semantic design. It is retained verbatim so the
fixed-width page rationale and the later change of direction remain recoverable.

## Persisted pages share the canonical whole-graph block quantum

All persisted outputs use one canonical persisted-page store abstraction regardless
of whether their producer is Tick or Tock. Pages are canonically aligned fixed-width
units whose width matches the fixed whole-graph root block size for the active layout
generation. This is a storage representation choice; semantic coverage remains
independent of sequential-input history and Tick-output history/latency.

For any persisted output page:

```text
page_interval(i) = [i * B, (i + 1) * B)
page_domain(i)   = page_interval(i) & output.coverage
```

A candidate page becomes valid only after its complete nonempty `page_domain` has
been materialized for the target `(semantic_version, page_version)` pair. One
`tock_coverage()` call may cover many selected pages; page boundaries do not imply one
callback per page.

Tick/persisted retention begins when a completed Tick invocation leaves its output
`written` or `voided`. Its addressed callback block enters the same persisted-page
store used by Tock/persisted output through the recording queue; no implicit recorder
or Tock callback is required. Random Access observes the data only through the
canonical published page view in the preliminary implementation.

Data representation may adapt to coverage occupancy: dense pages may store one value
per storage position, while coverage-packed pages store only covered positions. The
outer page directory should keep page-index order and small stable handles while large
data live in stable arena/chunk/file-backed storage.

### Changing the whole-graph block size repages stored data

Changing `B` changes storage partitioning, not DSP meaning. Persisted values are
losslessly repartitioned onto the new canonical page grid during a quiescent layout
transition. Sample values retain absolute indices; event data are repartitioned by
absolute index while preserving deterministic event order.

Tock/ephemeral outputs have no persisted data obligation requiring migration.
Tick-capture slabs are pre-publication/background-input storage, not a second
persisted representation. Recording and Tick/persisted staging may share the pool.
Consumed blocks become reclaimable after the transaction that consumes them commits,
subject to the callback-boundary rule for blocks visible to the audio thread. Old
immutable published snapshots may retain the old storage layout until their readers
release them.

## Stored event pages, fan-in order, and random-access event iteration

Persisted event outputs use whole-page candidate validity with packed event data.

A valid event page means the **complete event set for every position in the exact
covered page domain is known**, including the possibility of zero events.

`EventOutputProperties::max_events_per_index` remains density/capacity metadata, not
a literal per-timestamp quota. For one stored page:

```text
covered_positions      = measure(page_domain)
required_event_entries = ceil(max_events_per_index * covered_positions)
```

For a generated live event representation spanning `W` positions, the analogous
capacity uses the appropriate compiler-known effective fan-in bound over `W`. When
several event connections are combined into one buffer, GraphJit must account for all
incoming effective capacity bounds so every legal incoming sequence fits without
audio-thread allocation.

Background evaluation may dynamically commit persisted event data capacity off the
realtime thread. Tick-produced persisted events and explicit recording use
producer-specific reserve blocks supplied ahead of demand by the async capacity
manager; the realtime path consumes only already-assigned blocks and performs no
request-sized allocation. A future recent-data overlay could use recent queued event
payloads in front of published event pages, but the preliminary Random Access path
does not.

Event order is semantic and deterministic. Within one producer, events are emitted in
nondecreasing absolute sample-index order. Fan-in uses a stable tie break:

```text
1. absolute sample index
2. stable semantic source/connection index
3. producer-local event order
```

Page evaluation order, worker scheduling, cache/store layout, and request order must
never change equal-timestamp ordering.

Because one logical random-access event read can span multiple stored pages,
node-facing reads expose a segmented ordered range/iterator rather than promise one
contiguous `std::span<TimedEvent>`. Iteration preserves the ordering above and never
exposes events outside input coverage.

## Persisted page / persisted-page store glossary entry

A **persisted page** is the canonical retained/versioned storage unit for persisted
sample or event output data. **All persisted outputs use the same persisted-page store
abstraction regardless of whether their producer is Tick or Tock.** Production mode
changes how pages are filled; it does not create another retained-data read path.

Page validity is version-specific. An older readable persisted page may remain pinned
while a successor candidate is incomplete or invalid. Backing storage may still be
RAM, mmap/file-backed storage, immutable chunks, compression, or another page-store
backend, but those are implementations of the same persisted-page abstraction rather
than distinct semantic representations.

Do not call an ephemeral intermediate a persisted page.
