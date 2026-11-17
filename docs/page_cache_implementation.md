# Background page retention and reclamation

Status: active implementation, 2026-11-17. The owner confirms that the output
gain feature passes tests and works in the application. This is the next
complete feature. Playhead work follows owner verification of this feature.

## Accepted contract

The normative contract is in
[output capabilities](./output_capabilities_and_background_demand.md#reconstruction-and-cache-priority-owner-decision-2026-11-17).
Computed materializations favor reclamation unless expected reuse justifies
retention. Region/Coverage buffered contents favor retention within resource
budgets and can be reconstructed for the same semantic snapshot.
Sequential/buffered contents are authoritative recordings: their only
recoverable copy cannot be discarded. Reader pins and prepared execution
requirements prevent reclamation. Reclaiming payload never fabricates absence,
changes logical Regions or substitutes a newer input version.

## Implementation order

1. Trace the existing shared page allocator, immutable versions, mappings,
   validity metadata and transaction preparation. Record reconstruction inputs
   and ownership constraints before choosing eviction boundaries.
2. Implement page retention classification, admission and bounded background
   reclamation through the shared storage owner. Preserve recording bytes and
   pinned versions. Keep mapping, payload validity and logical coverage separate.
3. Integrate reconstruction and cache reuse into demand-scoped transactions,
   graph revision reuse, observation reads and playback preparation. No
   allocation, eviction, mapping or reconstruction enters the audio callback.
4. Add semantic and resource-pressure regressions after the implementation
   interfaces are established. Provide owner-run measurements if required.
5. Ask the owner to rebuild and run tests. Fix reported failures before
   starting the playhead feature.

No implementation change or test result is claimed by this plan. The owner
runs builds, tests, typechecks and profiling. The agent does not manipulate
Git staging or commits. Unexpected reconstruction, memory or dependency costs
must be reported before adding a workaround or changing the contract.

## Ownership friction found during source inspection

Observed source, 2026-11-17; no implementation change is claimed:

- `PersistedDataStore::Snapshot` owns the sample/event output directories.
  `SampleOutputState` and `EventOutputState` combine logical metadata with
  retained payload owners. `RealtimePersistedState::capture_current()` pins
  this entire Snapshot. Consequently a playback reader can retain unrelated
  cache contents, even though prepared mappings already have independent owners.
- `Snapshot::shares_output_with()` compares output-state owner identity.
  `BackgroundEvaluationTransaction` uses that result to detect changed recorded
  dependencies. Replacing an output state only to evict payload would also
  change this identity. Cache residency must not masquerade as a semantic edit.
- `BufferedPageAllocator::reserve_backing()` reclaims only already-unreferenced
  pages. It cannot discard valid cached contents retained by the directories.
  Evicting a successor directory cannot free those pages while the transaction's
  base Snapshot or the active playback Snapshot still owns them.
- `expand_output_demand()` can request missing persisted windows, but its
  reconstruction inputs come from the selected graph and data transaction.
  A payload cache entry does not itself retain an exact reconstruction request
  and its immutable dependencies. Such dependencies must survive eviction of
  results that remain reconstructible; latest recorded input is not equivalent.

Continuing with only a victim-selection policy would either reclaim no useful
backing under playback, invalidate unchanged dependencies, or lose the inputs
needed to reproduce a historical result. These are ownership and lifetime
constraints, not reasons to relax the accepted semantics.

Recommended feature boundary: separate immutable logical output/dependency
versions from reclaimable sample/event payload ownership. Background preparation
acquires explicit payload leases; playback keeps only its prepared leases.
Authoritative recording contents remain strongly owned. Reconstructible entries
retain exact reconstruction dependencies, and their retained dependency bytes
count toward admission/eviction decisions. Semantic identity remains stable when
payload residency changes. Then implement admission/reclamation and demand
reconstruction across `PersistedDataStore`, `RealtimePersistedState`,
`TickInvocationFrame`, `BackgroundStorageRealization` and
`BackgroundEvaluationTransaction` together.

This expands the ownership refactor required before step 2. There are no
intentional compiler errors and no source edits to verify yet. Resolve this
friction with the owner before selecting a narrower workaround. Playhead work
still follows completed implementation and owner verification.

Owner decision, 2026-11-17: include the broader ownership refactor in this
feature. The logical-version, payload-cache and prepared-reader lifetime split
is authorized. Implement it before the retention policy; do not ask again to
expand this same boundary. No builds or tests are agent-run.

## In-progress source checkpoint

Implementation is underway; no build or test has been run. This is not a
completion claim. `PersistedDataStore` now separates semantic output identity
from the payload retained by a Snapshot selection. Cache-only publication
preserves the data revision; semantic candidates still reject a competing
semantic publication. Prepared playback selects only its direct persisted
bindings; `TickMaterializationSnapshot` already owns its prepared payload.
Initial roots copy only the selected version into preallocated storage.

Background preparation now preflights sample/event backing demand, protects
required windows, and reclaims optional contents before allocating. Computed
outputs can use the same store without changing their authored capabilities.
The demand planner consults cache availability; missing known-covered windows
schedule production. Reconstruction records retain the exact executable and
authoritative recording dependencies, without retaining intermediate cache
roots recursively. Discovery readers are being made independent of mutable
private transaction cache selections.

Regressions added so far cover recording preservation, old reader contents,
selected-page lifetime, computed-before-buffered priority, partial required
windows, semantic candidate validity across eviction, reconstruction dependency
ownership, and recomputation of an evicted Tock window. These are unrun.

Remaining review: complete discovery ownership, event-chunk lease granularity,
incremental graph reuse and exact reconstruction invocation, pressure failure
paths, source comments/docs, and owner verification. Do not begin playhead work.

## Source ready for owner verification

The ownership refactor, policy integration and regressions are implemented.
They have not been compiled or run by the agent. `git diff --check` is clean.
The owner must verify the source before this feature is considered complete.

`Snapshot::shares_output_with()` now compares a separate semantic identity.
Reclamation preserves that identity and the published data revision. A private
candidate pins the identity of its semantic base, so a cache-only publication
does not make it stale; a competing semantic publication still does.
`ReaderPin::detach()`, `Snapshot::select_outputs()` and `retain_payload()` build
background-owned selections. Playback retains direct persisted bindings and
the payload already owned by its Tick materialization. Discovery sample readers
retain prepared windows, while event readers own their selected output.
Neither reader depends on a transaction root being left untouched by reclamation.

`prepare_cache()` estimates incoming backing requirements and protects the
requested output windows. It considers the shared physical budget, the optional
cache budget, and mapped-byte/run pressure. Recordings and external reader
leases remain outside its disposable ownership. Initial admission uses request
counts to establish reuse of computed results; buffered results favor retention.
Under pressure, computed contents precede buffered contents, with request
recency, reuse count and retained bytes refining victim order. These heuristics
and the 256 MiB optional-cache default are measurement choices. They are not
authored cost ratios. The hard allocator limits remain authoritative when
readers or recording dependencies prevent physical reclamation.

Computed caching currently admits native output representations. Channel
subsets read that same native payload. Derived conversion/composition buffers
stay transaction-scoped to avoid adding repacking work. This does not
grant computed outputs complete-buffer input compatibility. Sample selections
reclaim page subsets. Event sequences use their existing immutable output
ownership unit: a required sequence remains retained as a unit, rather than
copying variable-capacity event contents during eviction. Logical Region order,
duplicates, gaps and known-empty event results remain separate from validity.

Reconstruction records own the compiled executable and only the upstream
authoritative recording dependencies. Their pages share the normal allocator
accounting. `BackgroundEvaluationTransaction::reconstruct()` uses those exact
inputs in a private transaction. It cannot publish historical results over the
current canonical graph. Normal active-graph demands regenerate missing covered
windows through the same evaluation path; lack of payload is never fallback.
Generation reconciliation now retires optional computed caches too, releasing
their reconstruction owners when the outputs disappear or change representation.

Nine added regression cases cover output/page ownership, recorded input
versions, event metadata, pinned readers, resource failure and recomputation.
The initial-root regression now checks version capture without retaining an
unrequested recording. Existing storage-compatibility checks also cover computed
cache eligibility. No TypeScript or UI source changed.

Owner verification:

```sh
cmake --build build-release -j16
ctest --test-dir build-release -j16 --output-on-failure > build-release/Testing/Temporary/LastTest-verbatim.log
bash scripts/benchmark_page_cache.sh
```

The new benchmark mode is `iv_buffered_storage_benchmark --cache`. Its script
records results under `build-release/page-cache/` for 64/1024-page outputs and
zero/four pinned readers. It checks recording and reader contents, reports
preparation/publication/reclamation latency, live/backing bytes, eviction counts,
offset reuse and mapping calls. It does not time DSP or LLVM. The existing
`scripts/benchmark_buffered_storage.sh` remains available for the previous
overwrite/fragmentation comparison. Playhead implementation waits for successful
owner verification and correction of any failures.

## First owner verification and Tick-persistence correction

The owner rebuilt and ran 1,142 tests: 1,140 passed. The failures were
`GraphExecutorFixture.PersistedTickReplayPublishesOnlyTheDemandedPositionalRange`
(segfault) and
`BackgroundStorageRealization.TickProducedPersistenceOverwritesOnlyTheSelectedPositionalRange`
(an authored-Region representation exception).

Source review found that Sequential canonical persistence had been routed
through logical output Region selection, and that publication used the
`persisted_tick_output` planning flag instead of the storage/production contract.
The correction keeps `persisted_data` plus Sequential production positional,
including replay-produced writes. Logical output Regions remain appropriate for
optional computed replay caches, whose payload must be evictable independently
of their coverage. Both sample and event publication use the corrected rule.

The two existing regressions now exercise computed caching both enabled and
disabled. The agent has not rerun them or built the correction. The owner must
verify these changes; playhead work remains pending.

## Successful owner verification and measurements

The owner confirms all tests pass after the Tick-persistence correction and
ran `bash scripts/benchmark_page_cache.sh`. The reports under
`build-release/page-cache/` contain 100 iterations per case:

| Pages per output | Pinned readers | Median / p95 (µs) | Peak live (MiB) | Live after reader release (MiB) | Backing retained (MiB) |
| --- | --- | --- | --- | --- | --- |
| 64 | 0 | 156.589 / 191.708 | 1.25 | 1 | 1.25 |
| 64 | 4 | 203.316 / 238.019 | 1.75 | 1 | 1.75 |
| 1,024 | 0 | 1,682.46 / 2,133.94 | 20 | 16 | 20 |
| 1,024 | 4 | 1,629.91 / 1,917.52 | 28 | 16 | 28 |

Each case reports 97 evictions. Reused offsets account for 6,016–6,144 pages
in the smaller runs and 96,256–98,304 in the larger runs. The benchmark's
checks for unchanged recordings and pinned readers passed. Released live
payload returns to the same amount regardless of reader count. Unreferenced
physical backing remains reusable within the configured hard budget.

These are background write/publication/reclamation timings, not DSP callback
or LLVM measurements. They do not establish an optimal cache heuristic or a
performance guarantee for arbitrary project graphs. No blocker appears in
these runs. The owner-verified memory feature is the baseline for the next
authorized feature: project playback/playhead.
