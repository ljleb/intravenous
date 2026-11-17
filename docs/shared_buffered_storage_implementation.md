# Shared buffered-output storage implementation

Status: active implementation, 2026-11-16. The owner reports all current tests
pass and authorizes the next feature after the visualization refinements.
This is coordination, not a replacement for the contracts in
[Output capabilities, section 8](./output_capabilities_and_background_demand.md#8-shared-buffered-storage-sparse-mappings-and-resource-limits).
Builds, tests, typechecks and profiling remain owner-run.

Active successor, 2026-11-17: the owner authorizes the retention and reclamation
policy after successful output-gain verification. Follow
[the page-cache implementation plan](./page_cache_implementation.md). Playhead
work waits for owner verification of that feature.

Latest checkpoint, 2026-11-17: the owner reports all tests pass and has run
the storage benchmark. The results below verify the tested behaviour and expose
two representation costs to resolve before storage performance is consolidated.

Current follow-up status: the fresh measurements confirm the intended reductions.
A test-registration mistake discovered during review has been corrected; the
19 `SharedBufferedStorage` cases still need an explicit owner run. See the final
verification checkpoint below before treating the storage follow-up as complete.

The feature unifies Tick recording and Tock/buffered output storage. Both must
publish immutable versions with separate logical Regions, exact valid contents,
physical backing ownership and selected readable views. Reuse unchanged pages;
preserve old snapshots; reclaim or reuse backing only after all relevant owners
release it. Computed outputs retain their existing demand-scoped execution.

Implementation order within this complete feature:

1. Verify `PersistedDataStore` recording pages, publication and mapping pins,
   and the buffered publication paths in `background_storage_realization.cpp`.
   Record any concrete representation friction before choosing a workaround.
2. Generalize the existing page allocation/directory ownership into the shared
   buffered resource. Implement background-only reclamation and safe offset
   reuse, including partial-page copy-on-write and immutable reader pins.
3. Route Tock/buffered sample production and recording incorporation through
   that resource. Preserve event-buffer ownership/versioning under the same
   contract with storage appropriate to variable-capacity event contents.
4. Supply bounded windows and eligible contiguous pinned views from shared
   versions. Retain run-aware mappings and enforce independent per-view/total
   virtual-byte, physical-backing, process VMA/headroom and realtime residency
   budgets. Complete-buffer requests fail explicitly when they cannot be met.
5. Integrate preparation, graph cutover, observation queries and reclamation.
   No mapping, backing allocation, reclamation or missing-content resolution
   enters the audio callback. Authoritative recordings cannot be discarded as
   regenerable caches.
6. Add regressions after the implementation shape is established: sparse fills,
   partial-page overwrites/erasures, old snapshots and mapped readers, page/run
   sharing, bounded backing under repeated overwrites, fragmented runs and
   independent resource pressure. Provide owner-run measurements and required
   verification; refactor the completed implementation without changing semantics.

Concrete configuration names/defaults and backend choices are implementation
decisions within the normative resource contract. Any unresolved semantic
choice or unexpectedly expensive requirement is a friction report, not an
implicit change to that contract. Do not leave a recording-only allocation
engine beside a second Tock-buffered allocation engine.

## Source findings before implementation

`SampleOutputState` keeps `recording_pages`, `recording_runs` and eagerly mapped
`logical_regions` for positional recordings. Tock/buffered publication instead
copies transaction windows into heap-owned `PersistedSampleRegion` payloads.
Recording mappings retain the allocator and run tree but not page records;
reclaiming an offset therefore first requires fixing mapping ownership. Partial
recording erasure also retains its entire old page directory, so allocator
recycling alone cannot reclaim all obsolete pages.

Resource bounds require more than limiting `mmap()`: `Snapshot::sample_regions()`
and `sample_region_views()` currently expose ready pointers for all retained
Regions. A long authoritative recording must remain retainable when only a
bounded playback window is mapped. Logical storage and prepared read views need
distinct ownership, preserving Region ordering and overlaps. Background and
Tick readers must consume prepared views, never map on an audio-thread read.

`RealtimePersistedState::capture_current()` is intentionally a bounded atomic
pin after transaction commit. Mapping/residency preparation cannot be inserted
there. It must run on the candidate side before commit and move with the same
coherent prepared data/materialization selection. Failed background transactions
already retain selected producer-queue prefixes for retry; resource failures
must preserve that lossless recording behaviour.

## Valid-content retention decision

The 2026-11-16 implementation preserved every valid buffered payload until
invalidation. On 2026-11-17 the owner supplies a stronger reconstruction
contract: Sequential/buffered recordings are non-recomputable, while
Region/Coverage buffered results are recomputable for the same selected
snapshot and receive cache priority over computed intermediates. The old
policy is preserved [historically](./historical/buffered_reconstruction_before_2026-11-17/shared_buffered_storage_implementation.md).

The shared page store currently still preserves valid Tock payloads. Near-term
follow-up must classify authoritative recordings separately from recomputable
cached Tock contents, reclaim only unpinned reconstructible content, preserve
logical Regions/dependency versions, and schedule missing windows before use.
Add memory-pressure, reconstruction, cache-priority and reader-pin regressions.
Do not put eviction, reconstruction or allocation on the audio thread.
This follow-up follows the active input-union restoration/output-gain feature;
it is not claimed implemented by recording the new policy.

The owner's later clarification defines asymmetric retention priorities:
computed pages favor reclamation unless expected reuse justifies retention.
Region/Coverage buffered pages favor retention, subject to memory pressure and
reconstruction for the same snapshot. Sequential/buffered pages cannot lose
their only recoverable contents. Implement cache admission as well as eviction
under this [retention-cost contract](./output_capabilities_and_background_demand.md#retention-cost-and-memory-pressure-owner-clarification-2026-11-17).
Include reuse after incremental changes to background subgraphs in the policy.
Do not treat the priorities as fixed numerical cost estimates.

## Implementation checkpoint, 2026-11-17

Source and regressions are ready for owner verification. No build, test,
typecheck or benchmark has been run by the agent.

`BufferedPageAllocator` in `runtime/buffered_storage.{h,cpp}` now owns the shared
Linux page file and resource accounting. `PersistedDataStore` uses it for both
positional recording and Tock/buffered sample publication. Native-layout sample
pages are immutable while referenced. Replacements compare page contents and
share unchanged pages; mapped views retain their exact page subset, independently
of snapshot ownership. Recording voids remove pages without valid neighbours.
Snapshot retirement and the last mapped reader release make obsolete offsets
reusable on the background path.

Unused pages normally remain cached for cheap reuse. When backing pressure needs
that space, the allocator punches holes only in unreferenced cached pages. It
retains the empty file slots for reuse. File extent, physically retained sample
backing, cached backing, live backing and event capacity therefore have separate
usage counters. Valid buffered contents are never reclaimed by this mechanism.
An unchanged physical page can remain shared across graph/data revisions.

Events retain their immutable variable-capacity chunks and publication semantics.
Their retained vector capacities, including stored timeline copies, use the same
physical-payload budget. They are not forced into fixed-width sample pages.

`BufferedSampleBacking` separates physical ownership from
`BufferedRegionMapping`. Recording Regions retain their logical identity while
their readable payload is divided into bounded mappings. The initial preferred
window is at most 1 MiB and is also constrained by the configured per-view limit.
`PersistedSampleRegion::selected_view()` prepares a subset;
`contiguous_view()` requires actual complete contents and an allowed contiguous
mapping. `PersistedSampleReader` retains selected views rather than an entire old
output. Contiguous file runs are mapped together. Copying one mapping owner does
not create another mapping.

`TickMaterializationSnapshot::prepare_resident()` runs before transaction
publication. It touches/locks the selected sample/event payload through a
reference-counted residency lease, including retained carry-forward windows.
Overlapping leases remain locked until their last owner releases them. Pinning
failure aborts preparation; selected recording queue prefixes remain available
for retry. `RealtimePersistedState::capture_current()` remains an atomic pin.
`GraphExecutor::BackgroundExecutor` reclaims retired snapshots before ordinary
work and observation-window queries, so unpinned retired versions do not consume
the next transaction's budget indefinitely.

### Resource controls and measurement boundaries

`BufferedStorageLimits` is configurable through `PersistedDataStore` and the
third `GraphExecutor` constructor argument. Initial defaults are provisional:

| Resource | Initial limit |
| --- | --- |
| One contiguous mapping | 1 GiB |
| Aggregate mapped bytes | 4 GiB |
| Retained sample backing plus event payload capacity | 1 GiB |
| Aggregate mapped file runs | 16,384 |
| Process VMA headroom | 512 mappings |
| Prepared realtime payload pages | 64 MiB |

Actual process VMA count, Linux mapping limits and address-space limits are
checked independently. `mlock()` also obeys the host's `RLIMIT_MEMLOCK`; a lower
host limit can reject a selection before the configured residency budget.
Errors name the exhausted resource and leave the published version unchanged.
These are storage-owner controls, not new node declarations or UI settings.

This backend retains mappings for the current canonical materialized windows;
it does not yet evict those mappings while retaining their backing. Both are
separate owners, and obsolete prepared views can retire independently. There is
no eviction policy for valid reconstructible contents in this feature.

The payload budget is not a bound on total process RSS: transaction scratch,
Region/index metadata and allocator bookkeeping are separate. Multichannel
planar slices still need repacking when the native contiguous plane stride
changes; interleaved and mono slices can directly map the selected backing.
Run fragmentation, mapping preparation, the 1 MiB window preference and retained
snapshot costs need measurements. No speedup is claimed before those runs.

### Owner verification

Use the normal release rebuild and full test command. New regressions are in
`tests/shared_buffered_storage_gtest.cpp`, plus the queued-recording retry case
in `tests/persisted_data_store_gtest.cpp`. They cover reuse after retirement,
mapped readers outliving snapshots, partial-page voids, immutable old values,
unchanged-page sharing, bounded/complete views, circular indices, planar channels,
backing pressure, VMA/run limits, event accounting and residency lease lifetime.

After building, run:

```sh
scripts/benchmark_buffered_storage.sh
```

This invokes `iv_buffered_storage_benchmark` without rebuilding anything and
overwrites eight TSV reports under `build-release/buffered-storage/`. It varies
64/1,024 pages, zero/four pinned old versions and 64/1,024 KiB per-view limits.
Each invocation measures dense and fragmented replacements for recording and
Tock storage. An optional first argument changes the default 100 iterations;
`IV_BUILD_DIR` selects another build directory.

Rows report median/p95 background write/publication/reclamation time, live and
cached backing, file extent, reusable offsets, observed mapped-byte/run peaks,
sampled process VMA counts and mapping/reuse counters. Payload construction and
usage-counter sampling are included in the timed write path. No DSP/LLVM or
audio callback runs here; residency failures are covered by the tests. These
measurements characterize this backend, not a before/after audio-performance
claim. Review the larger application benchmarks if they reveal playback effects.

## Owner verification and measured friction, 2026-11-17

The owner reports all tests pass. The retained
`build-release/Testing/Temporary/LastTest-verbatim.log` records 1,089 backend
tests, zero failures, and 93.40 seconds wall time. All eight storage reports
are present under `build-release/buffered-storage/`, each with four completed
cases and 100 iterations. The agent read these artifacts; it did not rerun them.

For a 4 MiB logical payload with no pinned old snapshots, median background
write/publication/reclamation times are:

| Operation | 1 MiB view limit | 64 KiB view limit |
| --- | ---: | ---: |
| Dense recording replacement | 6.073 ms | 20.665 ms |
| Dense Tock replacement | 5.390 ms | 17.890 ms |
| One-Sample recording replacement | 97.601 us | 76.721 us |
| One-Sample Tock replacement | 221.153 us | 279.623 us |

These rows come from `pages-1024-pins-0-view-{1024,64}-KiB.tsv`.
With four pinned old snapshots, dense recording replacement rises to 9.115 ms
with 1 MiB views and 61.298 ms with 64 KiB views. This is background storage
work, not measured audio-callback time or end-to-end graph-edit latency.

Recording reuse behaves as expected in these runs. After the fragmented 4 MiB
case retires its readers, live page contents remain 4 MiB. Retained sample backing
is 4 MiB plus one cached 4 KiB page without old pins, or plus five cached pages
after the four-pin run. Dense recording and Tock replacement retain 8 MiB of
sample backing without old pins and 24 MiB after the four-pin runs; after pins
retire, the excess is reusable cached backing, not live old versions. These are
observed bounds for this workload, not proof for every workload.

### Friction 1: reuse loses contiguous file runs

**Planned assumption:** recycling obsolete offsets should avoid file growth
without turning otherwise contiguous dense writes into many small mappings.

**Observed source and measurements:** `BufferedPageAllocator::release()` pushes
individual records onto `free_`, and `allocate()` consumes that list without
preserving file-offset order. `map_buffered_region()` correctly coalesces
contiguous physical runs, but it cannot coalesce noncontiguous offsets. In the
4 MiB/no-pin/1 MiB-view dense recording run, the reported peak is 2,038 mapped
runs and 99,477 mapping calls over 100 iterations. The corresponding Tock row
reports 2,024 runs and 92,620 mapping calls. Reuse keeps backing bounded while
producing fragmentation that the mapping layer must then represent.

`reserve_mapping()` also inspects `/proc/self/maps` for each new view. Smaller
views repeat that inspection more often. The timings show that the smaller-view
tradeoff differs between dense and small writes; they do not establish how much
time belongs to each syscall, index update or process-map scan.

**Reach and recommendation:** give the shared allocator run-aware reuse, then
measure whether page writes and view-budget inspection should be batched. This
affects both recording and Tock sample publication, mapping/run accounting and
the benchmark. Retain actual process-headroom checks and immutable page pins;
raising resource limits alone would not remove the work. A simpler alternative
is to retain the current allocator until real projects demonstrate pressure,
but it leaves a measured representation cost in the common backend.

### Friction 2: Tock patches allocate backing per prepared window

**Planned assumption:** a partial replacement should replace the affected page
of a logical Region while retaining its unchanged contents.

**Observed source and measurements:** `replace_sample_regions()` retains the
old windows outside the replacement domain and sends new windows through
`page_backed_region()`. That function builds backing for each payload window;
it zeroes bytes outside that window and compares the whole page to the prior
page. A one-Sample patch therefore needs a new page while the old page remains
referenced for its other values. Different prepared windows can retain different
physical pages for positions within the same logical page.

In both 4 MiB/no-pin fragmented Tock runs, 100 writes to distinct pages leave
4,603,904 live bytes: the original 4 MiB plus 100 additional 4 KiB pages.
In the 256 KiB case, the pattern visits all 64 pages and leaves 512 KiB live.
This remains live backing after reader retirement, rather than reusable cache.
It is representation overhead, not evidence that unreachable owners leaked.
The existing sparse-patch test does not establish the cost of many edits at
different positions within one page.

**Reach and recommendation:** retain page backing per logical Region and make
prepared windows select from that backing. Patching a page must preserve other
valid bytes through copy-on-write, retain exact materialization validity and
keep overlapping/duplicate logical Regions independent. Native planar layout,
bounded views, complete-buffer views, background readers and old snapshot pins
must participate in that change. Merely merging logical coverage or dropping
old windows would lose valid contents. Increasing the budget would only postpone
pressure. Add measurements for repeated patches within the same page and
cross-page patches, including multichannel planar values.

### Proposed continuation

The tested storage feature works, but these results do not justify treating its
performance as consolidated. Resolve run-aware reuse first, then Tock backing
ownership and the related measurements, before output gain and graph-authored
snapping. This is a recommendation based on measured friction, not a newly
accepted architecture change. No source implementation is changed in this
review, and no claim of audio-thread speedup follows from these storage runs.

## Approved follow-up implementation, 2026-11-17

The owner approves both measured fixes. Keep the shared allocator's free offsets
ordered without allocating on last release. Then retain one immutable page
directory per logical Tock Region and apply partial replacements to that
directory. Prepared windows select readable views from the resulting version;
their boundaries must not determine canonical page ownership. Preserve duplicate
and overlapping Region independence, exact validity, native channel layouts,
old reader pins, resource failures and background-only preparation. Extend the
regressions and owner-run benchmark to same-page, cross-page and planar patches.
Builds, tests and measurements remain owner-run.

### Approved fixes implemented; owner verification pending

`BufferedPageAllocator` now keeps cached and punched offsets in intrusive AVL
trees ordered by file offset. Page records contain the tree links, so last
release still allocates no metadata. Returning or selecting a free page costs
logarithmic tree work on the background path. Dense writes consume available
offsets in increasing order and can retain contiguous physical runs. Page pins,
hole punching and all resource checks retain their existing ownership rules.

`replace_tock_region()` owns one persistent page directory per authored logical
Region. Coordinates are relative to that Region's origin. Planar channel offsets
remain stable across requests; larger planes use page-aligned channel origins,
while a complete small payload that fits one page can share that page. Duplicate
and overlapping logical Regions retain independent directories and values.

`write_tock_windows()` sorts incoming byte spans and streams them through one
scratch page. Disjoint windows touching that page are applied before one
copy-on-write allocation. Equal bytes preserve the old page. Erasure prunes
pages only when no valid contents remain on them. Logical coverage and exact
materialized validity remain separate from allocation and prepared mappings.

Prepared views are rebuilt from the resulting valid positions with stable,
bounded chunk boundaries. An index of previous view ranges avoids a quadratic
scan when many sparse windows exist. Views whose physical source pages are
unchanged remain shared. A same-page modification remaps other views of that
page to the successor page, rather than retaining obsolete pages merely to keep
their pointer identity.

Planar views translate canonical channel runs into a contiguous native-layout
mapping when page boundaries permit it. The mapping pins only its selected
canonical page subset. Partial planes that cannot share a virtual page require
a bounded native-layout repack; the prepared copy and canonical contents have
separate owners. This does not put copying, mapping or allocation on the audio
thread. Both forms remain subject to resource limits and preserve old readers.

New regressions cover ordered dense reuse with zero/four old pins, repeated
same-page and cross-page updates, several incoming windows on one page, sparse
validity, planar page mapping and bounded repacking, unchanged-view reuse,
selected views outliving snapshots, and overlapping/duplicate Region isolation.
The earlier huge-Region sharing test now places its second window on another
page; dedicated tests cover same-page copy-on-write and retirement.

The owner-run benchmark retains its four original cases for comparison with
the baseline recorded above. It adds mono same-page/cross-page patches and four
stereo planar cases: dense, scattered, same-page and cross-page updates. The
script still writes eight reports, now normally containing ten rows each.
`pages_per_channel`, `channels` and `payload_bytes` identify the actual size.
Each case checks its final value in every channel and rejects excess live pages
after old readers retire for these fully valid, page-aligned workloads.

Rebuild and run the backend suite, then rerun
`scripts/benchmark_buffered_storage.sh`. Compare dense mapping-call/run counts
and times, and check that the scattered Tock cases retain exactly their payload
in live pages after retirement. Process-map inspection still occurs for each
new mapping; batching remains a measurement-led follow-up if it is still costly.
No new performance result or passing test result is claimed yet. The agent has
performed source inspection and static diff checks only.

## Follow-up measurements and test-registration correction, 2026-11-17

The owner reports the rebuilt suite passes and reran the benchmark. The retained
CTest log records 1,089 passing registered cases in 90.09 seconds. All eight
storage reports contain ten completed cases with 100 iterations each. In all
80 rows, `live_bytes` equals `payload_bytes` after old reader retirement.
The agent only read and checked the artifacts; it did not rerun binaries.

Observed before/after medians for a 4 MiB mono payload and no pinned old versions:

| Operation / mapping limit | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Dense recording / 1 MiB | 6.073 ms | 2.239 ms | 63.1% |
| Dense Tock / 1 MiB | 5.390 ms | 2.092 ms | 61.2% |
| Scattered one-Sample Tock / 1 MiB | 221.153 us | 89.226 us | 59.7% |
| Dense recording / 64 KiB | 20.665 ms | 5.332 ms | 74.2% |
| Dense Tock / 64 KiB | 17.890 ms | 5.386 ms | 69.9% |

With four old versions pinned, dense recording with 64 KiB views falls from
61.298 ms to 8.206 ms (86.6%). These are observations from the retained runs,
not statistical confidence bounds or audio-callback timings.

For dense recording with 1 MiB views and no old pins, mapping calls across
100 writes fall from 99,477 to 800, and the observed peak mapped-run count falls
from 2,038 to 8. Dense Tock falls from 92,620 calls to 800 and from 2,024 runs
to 8. The 4 MiB scattered Tock case now retains 4,194,304 live bytes instead of
4,603,904; the 256 KiB case retains 262,144 instead of 524,288. Cached reusable
backing remains separately accounted, so these are live-content comparisons.

The added same-page, cross-page and stereo planar cases all complete. For
1 MiB views and no old pins, the 4 MiB mono same-page median is 35.644 us and
cross-page median is 100.015 us. Stereo cases contain an 8 MiB payload: their
same-page and cross-page medians are 43.685 us and 135.251 us. The benchmark's
final-value and live-page checks pass in every row. Small mapping windows remain
more expensive for dense writes; optional batching is still a future tuning
candidate, not a prerequisite for moving on after correctness verification.

### Registration mistake and required verification

The agent found an error in its original CMake integration. `add_iv_gtest()`
calls configure-time `gtest_add_tests()`. The shared-storage source had been
attached with `target_sources()` only after that scan, so its tests compiled
into the executable but had no CTest entries. Neither the current 1,089-case
run nor earlier runs through that registration covered these 19 cases. The
separate queued-recording retry regression in `persisted_data_store_gtest.cpp`
was registered normally. Earlier statements about passing suites must be read
with this limitation; compiled test code alone was not execution evidence.

`add_iv_gtest()` now accepts `EXTRA_SOURCES` and includes them in the executable
before discovery and label assignment. The shared-storage file uses that route.
The next owner build regenerates the CTest entries and tracks the extra source
for future configure-time rediscovery. No DSP/storage implementation changed
during this review.

Run the focused cases after rebuilding the existing test target:

```sh
cmake --build build-release --target intravenous_persisted_data_store_tests -j16
ctest --test-dir build-release -R '^SharedBufferedStorage\.' --output-on-failure
```

Expect 19 selected cases. If no other test sources changed, the next full suite
should contain 1,108 cases. The storage benchmark need not be rerun for this
CMake-only correction. Keep this follow-up pending until those regressions have
actually run; then output gain is the next accepted feature. No fresh build or
test execution was performed by the agent.
