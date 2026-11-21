# Output Capabilities, Buffer Access, And Background Demand

_Status: normative destination, accepted 2026-11-16. This contract supersedes
previous intent treating `ephemeral`/`persisted` as mandatory cache/retention
policies, treating `TickOutputConfig` as the only sequential output form, and
requiring complete logical Regions or complete persisted coverage to be
materialized merely because they exist. The superseded statements are preserved
[historically](./historical/output_ports_before_computed_buffered_2026-11-16.md).
This document describes the intended API and executor, **not** behavior already
implemented in the checked-in source._

## 1. Three separate authored questions

A port declaration defines what kind of connection is possible and what the
consumer can require, rather than choosing an executor cache policy.

| Question | Alternatives | Contract |
| --- | --- | --- |
| Input access | `SequentialInputConfig`, `RegionInputConfig`, `CoverageInputConfig` | How the consumer is permitted to address the incoming data |
| Output production/shape | `SequentialOutputConfig`, `RegionOutputConfig`, `CoverageOutputConfig` | What logical data shape the producer provides |
| Output realization | `computed`, `buffered` | Whether the port supplies values through computation or provides an independently addressable buffer-backed data resource |

The intended names for the final C++ configuration are
`OutputRealization::{computed, buffered}` and
`SequentialOutputConfig<Types...>` (replacing
`OutputRetention::{ephemeral, persisted}` and `TickOutputConfig<Types...>`).
`OutputProduction::{sequential, region, coverage}` replaces the `tick` production
alternative. Names for currently implemented source records remain historical
implementation facts until the API migration is completed.

**Tick and Tock have disjoint output shapes and share realization modes.**
A Tick primitive executes through `tick()`/`tick_block()` on the realtime graph
and declares only Sequential outputs. A Tock primitive executes through
`tock_coverage()` in the background and declares only Region or Coverage outputs.
Both use `OutputRealization::{computed, buffered}`. One concrete primitive has
one primary callback domain; a module can compose both.

| Callback domain | Sequential output | Region output | Coverage output |
| --- | --- | --- | --- |
| Tick | yes | no | no |
| Tock | no | yes | yes |

This matrix is an accepted correction to the initial output-capability revision.
The superseded cross-domain combinations are preserved
[historically](./historical/output_shape_symmetry_before_correction.md).
Output realization does not select a callback domain, cache policy or memory
allocation strategy. A destination's access mode does not change its source's
output shape.

## 2. `computed` and `buffered` are connection capabilities

A **computed output** supplies values through an executable computation, which
may define values over very large or full-circle `GlobalIndex` ranges. Its
covered extent does **not** entail an allocation proportional to that extent.
The executor may materialize bounded results into scratch or shared caches;
that does not turn the authored output into `buffered`. A computed output does
not promise a pointer to a complete logical Region. A computed Tick output is
not automatically replayable: only a valid contextual replay path can satisfy
background demand at arbitrary indices.

A **buffered output** exposes values as an independently accessible buffer-backed
resource. Values may be populated incrementally, overwritten, erased, or
regenerated according to the producing node's semantics. A Region-shaped
buffered output can satisfy a consumer demanding one complete contiguous Region
buffer **once that Region is actually prepared and pinned**. The buffer may be
reclaimed and regenerated when that is semantically sound. It need not exist
until demanded, and buffered does not mean eager production or permanent RAM
residency.

These are two configurations of the *same output axis* for Tick and Tock:

| Callback | Example output | Meaning |
| --- | --- | --- |
| Tick | Sequential/computed | Ordinary live Sequential stream; any background replay requires contextual proof |
| Tick | Sequential/buffered | Recording: live Sequential production writes/erases an asynchronously published data resource |
| Tock | Region or Coverage/computed | Position-addressable procedural data; only required portions are calculated |
| Tock | Region or Coverage/buffered | Buffer-backed data populated for required ranges, with complete-buffer capability when explicitly demanded |

Sample and event transport have their separate type/capacity/ordering rules; the
same shape/realization distinction applies to both. Region identities and
multiplicities remain distinct from the positional `IndexCoverage` union.

**Recording requires no third output mode.** A Tick Sequential/buffered output
updates a stored data resource as realtime blocks execute. The background actor
incorporates these writes into coherent immutable data snapshots. Its published
recording is Coverage-shaped for historical reads: arbitrary overwritten Tick
blocks do not create one authored Region. A Tick Sequential/buffered output can
therefore feed a Coverage input through its published recording, never a
Region input simply because its current stored extent is contiguous.

### Reconstruction and cache priority (owner decision, 2026-11-17)

The background executor distinguishes output shapes when deciding what it can
regenerate:

- **Sequential/buffered:** committed contents are non-recomputable recordings.
  A cache or RAM mapping may be dropped only if the authoritative bytes remain
  recoverable elsewhere. Do not replay the producer to invent lost recording
  data, even when a related computation is intrinsically replayable.
- **Region/buffered and Coverage/buffered:** contents are recomputable for the
  same selected semantic/input snapshot. The executor may reclaim unpinned
  cached payload and regenerate requested windows through Tock. Preserve logical
  Region identities, coverage, and snapshot dependencies while doing so.
- **Computed results:** cached content is also disposable where reproduction is
  permitted. Prefer retaining buffered Region/Coverage content over computed
  intermediates when selecting cache victims. This is a relative cache priority,
  not unlimited retention or permission to invalidate pinned reader contents.

Reconstruction must reproduce the same values and Region structure. It cannot
substitute a newer configuration or newer recorded inputs. Missing known-covered
content still requires preparation; it never becomes absence or fallback.
This decision replaces the earlier conservative policy that retained every
valid Tock/buffered page until explicit invalidation. Prior wording is preserved
[historically](./historical/buffered_reconstruction_before_2026-11-17/output_capabilities_and_background_demand.md).
The differentiated eviction policy and ownership refactor are now in source,
awaiting owner verification. See the
[implementation and verification record](./page_cache_implementation.md).

#### Retention cost and memory pressure (owner clarification, 2026-11-17)

`computed` and `buffered` express different default retention priorities.
They do not specify measured execution costs or permanent RAM residency.
The executor applies these priorities to optional background pages:

- Computed contents favor reclamation after the current demand and reader pins
  end. Retain them only when expected reuse justifies the memory cost. Reuse
  includes avoiding repeated work after incremental changes to a background
  subgraph, not only repeated reads of the same final output.
- Region/Coverage buffered contents favor retention while valid and within
  resource budgets. Memory pressure can reclaim unpinned contents that the
  executor can reconstruct for the same semantic snapshot. Give these pages
  greater retention priority than comparable computed intermediates.
- Sequential/buffered contents are authoritative recordings. Memory pressure
  cannot discard their only recoverable copy. The executor can release a RAM
  page only when another owner or backing resource preserves its contents.
  Removing recording data requires a separate semantic operation.

The executor considers expected reuse, reconstruction work, retained dependency
cost, page size and current memory pressure. These factors refine cache
admission and eviction within the default priorities. They do not grant Tick
replay permission or make a missing reconstruction dependency available.
No fixed cost ratio follows from the output declaration.

Reader pins and imminent execution requirements constrain every eviction.
If the executor cannot preserve required data within its resource budgets, it
must report a resource failure. It must not silently discard recordings or
allocate without bounds. Reclamation does not remove logical coverage or Region
identity. Later covered reads require preparation before execution, never
fallback merely because cached contents were reclaimed.

This clarification refines the relative cache priority above. The admission and
eviction implementation awaits owner verification; its initial heuristics are
measurement choices recorded in the implementation plan.

## 3. Input capability is a distinct compatibility constraint

A `RegionInputConfig` accepts exactly one logical Region; a
`CoverageInputConfig` accepts zero or more Regions, retaining overlap,
duplicates and identities. **Coverage outputs never connect directly to Region
inputs.** A conversion that chooses or constructs a Region is an explicit node,
not an implicit adapter based on the current Region count. A Region output may
feed a Coverage input as a one-Region collection.

A Region or Coverage input must also declare what *content access* it needs.
This is an access capability/constraint on the existing input config, not a new
output mode or a second kind of Region:

- **Bounded/windowed access:** full logical metadata (range, identity, Region
  order) remains visible, but content is accessible only within prepared,
  explicitly requested windows. A window may provide a contiguous span of its
  own. It does not provide a pointer spanning the full logical Region.
- **Complete-buffer access:** the callback requires a complete contiguous sample
  Region (or complete contiguous payload for each selected Region in Coverage)
  with pointer and size. Every such buffer must be prepared before that
  callback. A complete-buffer requirement can be met directly only by an
  appropriately shaped **buffered** output.

The initial Tick contract requires **complete-buffer access for every Region or
Coverage input**, including inputs on outputless sinks. Their selected entire
logical Regions are explicit background preparation demands. Tick callbacks may
not issue windowed Random Access reads. Windowed Region/Coverage inputs are
available to Tock callbacks. This restriction does not affect Tick Sequential
inputs, which continue to consume bounded prepared playback windows.

The authored input declaration uses `InputContentAccess::windowed` (the default)
or `InputContentAccess::complete_buffer`, for example:

```cpp
iv::input<iv::Sample>("clip", iv::InputAccess::region,
    iv::InputContentAccess::complete_buffer)
iv::event_input("points", iv::EventTypeId::sample, iv::InputAccess::coverage,
    iv::InputContentAccess::windowed)
```

`region.range()` remains the complete logical range. A windowed continuous
Region exposes `region.prepared_windows()` for the requested content spans;
these spans do not become new logical Regions. Random Access event reads name
an explicitly prepared range. A cached complete buffer cannot grant a later
windowed invocation access beyond its explicit request.

The shape and realization compatibility rules for direct source connections are:

| Source output | Sequential input | Windowed Region input | Complete-buffer Region input | Coverage input |
| --- | --- | --- | --- | --- |
| Region/computed | prepared bounded playback | yes | **no** | yes, with windowed content access |
| Region/buffered | prepared bounded playback | yes | yes, after complete preparation | yes |
| Coverage/computed | prepared bounded reduction | **no** | **no** | yes, with windowed content access |
| Coverage/buffered | prepared bounded reduction | **no** | **no** | yes, with windowed or prepared-buffer access |
| Sequential/computed | direct live streaming or prepared window | **no** | **no** | only if an eligible background replay/conversion provides addressable values |
| Sequential/buffered | direct live streaming or prepared window | **no** | **no** | through the applicable published recorded/addressable data |

A Coverage input demanding full buffers must be satisfied by appropriate
buffered **Region members**; merely declaring Coverage/buffered does not
collapse its many Regions into a single contiguous Region. A source's shape
cannot be silently changed to meet an incompatible destination.

A computed source requiring complete-buffer access must pass through an
**explicit materialization node** that authors a bounded Region (or selected
Coverage Regions) and produces a buffered output. The materialization boundary
makes the chosen extent and potentially expensive allocation visible in the
graph. Direct connection validation rejects a computed-to-complete-buffer edge
rather than secretly allocating the complete procedural domain.

Valid shape/realization alone is insufficient: value type, pace, channel
projection, coverage ordering, realtime availability, latency, and version
consistency still have to be satisfied.

## 4. Logical extent does not determine computation or allocation

Keep four quantities separate:

1. **Logical output Regions and exact `IndexCoverage`**: the positions at which
   values exist and the identities/decomposition of those positions.
2. **Requested positions**: positions demanded by a consumer, including
   prefetch and direct result queries.
3. **Required dependencies**: positions/information needed to compute that
   demand, discovered through `propagate_reverse_coverage()` and the suspendable
   coverage planner. The requirement may be wider than the output window.
4. **Materialized and valid positions**: the subset actually computed,
   initialized and safe to expose for the selected semantic/data version.

Neither `computed` nor `buffered` mandates generating an entire logical Region
or the entire covered output. A request intersecting a Region preserves that
Region's **full metadata and identity**, but its evaluation normally writes only
the demanded covered subranges. Clipped subranges are writable/readable windows,
**not** new logical Regions. The node may request broader inputs or output
work if its actual algorithm requires that (e.g. a nonlocal statistic or
whole-buffer FFT). Such widening must follow explicit dependency/complete-buffer
requirements, never Region membership alone.

`propagate_forward_coverage()` establishes output existence/Region lists and
conservative changes, potentially suspending on prepared discovery inputs.
`propagate_reverse_coverage()` establishes the necessary input information for
requested output positions. Background value evaluation executes once all the
required data are ready, using deliberately selected, version-consistent input
views. Prepared data from unrelated cached ranges never silently expand the
callback's selected input Regions/windows. If an algorithm requires complete
Region data, that requirement is made explicit in its input capability and
reverse demand.

A Tock/Region/computed Bézier segment can span an enormous circular interval
while a Sequential consumer requests the next 512 samples. The background
executor computes and stages that bounded window, not the full segment. A
Tock/Region/buffered producer may likewise compute only 512 samples unless a
consumer requires a full contiguous Region. A full-Region request that exceeds
available resources must fail explicitly, not attempt unbounded allocation.

## 5. Background executor: demand, retention, and correctness

The background executor owns **cache policy**, including how long reusable
computed blocks or buffered realizations are kept. Neither realization mode
means `cache forever` or `never cache`. Cache occupancy cannot alter the
semantically selected input Regions, required positions, or computed results.

A buffered output's published covered range is **not** a promise that every
position has already been computed. Its materialized-valid subset is separately
tracked. Each prepared window becomes available only when all required values
are valid for one coherent semantic/data version. Demand-driven publication can
reuse old valid portions, compute missing portions and retain/share them, without
forcing the rest of the covered domain to be evaluated. An uncomputed-but-covered
position is neither absent nor a neutral sample; requesting it schedules work
and a realtime attempt to use it before readiness is an underrun.

Cache eviction is permitted only if it leaves the selected version
reconstructible or preserves the authoritative data elsewhere. For example,
Tick Sequential/buffered recording writes are independent observed inputs to
future background computation; those values cannot be recomputed from graph
configuration alone. Their authoritative committed contents must survive
cache eviction, even if a particular RAM mapping or prepared buffer is dropped.
Recorded data can reside in another backing store. There is no separate
`recording` port kind, and no authored cache-retention flag.

The determinism requirement is: **given the same graph configuration, selected
input/recording data snapshot, and requested positions, background production
must yield the same semantic values and Region structure**, independently of
cache history, request batching or worker scheduling. Graph configuration alone
cannot reproduce a live microphone recording; its committed data snapshot is
an explicit semantic input.

The executor may prefetch, fuse, reuse and evict prepared windows based on its
cost model. These optimizations cannot authorize value changes, unrecorded
historical reads or premature visibility. Do not copy all output coverage merely
because a consumer requests a small part of it.

## 6. Audio thread, Sequential production, and feedback

Tick callbacks produce ordinary values at executed positions, but computed
values and logical output coverage are separate. Acyclic `input_union` Tick
nodes derive coverage from their inputs; `full` nodes declare full coverage.
Sequential consumers apply their own fallback where that output is absent,
even when the upstream Tick callback computed a fallback value there. The
[recovered coverage contract](./tick_input_union_and_feedback_direction.md)
corrects the November 16 full-only wording, preserved
[historically](./historical/tick_coverage_before_intent_recovery_2026-11-17/output_capabilities_and_background_demand.md).

A `.detach()` connection is a stateful Sequential feedback delay with initialized
history. It belongs to realtime scheduling, not background replay. Even if its
neighboring Tick computations qualify intrinsically for replay, an unresolved
feedback SCC cannot be contextually replayed through its stateful delay.
GraphJit schedules the feedback SCC on the audio thread. Its Tick members have
effective full live coverage regardless of their declared replay mode. Their
declarations remain unchanged. No recursive coverage solver or independent
full-contribution proof is required. Explicit semantic SCC membership excludes
ordinary contextual background replay; it does not invalidate an input-union
declaration. The override is local to feedback SCCs.

Tick callbacks write only their scheduled, preallocated Sequential output data.
Their published recording may expose historical Coverage, but the authored
output remains Sequential. Recording or contextual replay is required before
those values can support historical random access.

A Tock Region or Coverage output (including sparse automation) supplies prepared
bounded windows to a Tick consumer's Sequential input. Circular output Coverage
may contain gaps; the consuming Sequential input applies fallback only at
known-absent positions. Known-covered but unavailable data are an underrun,
not absence. The background actor prepares required windows before a pending
running seek/resume is applied. Current applied playback continues during a
pending running seek; preparation must not evict or starve that current demand.

If an active graph outruns required preparation, its requested foreground Tick block
cannot run: do not execute a partial root, fill the missing input with neutral values,
advance its Sequential history/`ctx.index`/playhead, or commit recording from that
unsuccessful block. A missed audio-device deadline is reported as an underrun and can
produce a hardware dropout outside graph semantics. Waiting for readiness may miss that
deadline; it must not indefinitely obstruct control commands, graph replacement or
shutdown. No realtime allocation, Random Access discovery or Tock invocation is
permitted in Tick code. See
[Project Playback And Presentation Grids](./project_playback_and_presentation_grids_direction.md#background-preparation-pending-seeks-and-missed-deadlines).

An ordinary Sequential consumer must not force an entire upstream Region or
buffer to be computed. It consumes a bounded sliding/ring window whose
capacity and refill lead are planned to satisfy its latency and dependency
constraints. A realtime Random Access consumer is more constrained: every
window it may access must be prepared and pinned **before** the Tick callback;
unpredictable live-address reads cannot block on background work. If their
preparation cannot be bounded, the graph requires an explicit alternative
(materialization, recorded source or another authored algorithm) or is
rejected at validation.

## 7. Publication and realization boundaries

- A `Region` remains a logical range/identity even when only some physical
  windows have been materialized. `Coverage` retains each Region separately,
  including overlap and duplicates; `IndexCoverage` is the positional union.
- A complete-buffer input promise is only fulfilled when **the selected entire
  Region buffer** exists, is initialized and pinned for the callback. An
  arbitrary huge computed output cannot acquire that promise implicitly.
- An immutable published data root may contain partially materialized buffered
  outputs, with exact validity metadata. Readers see only complete prepared
  windows. A candidate must not expose incomplete required results or mix
  semantic/data versions. Completion is **relative to requested demands**, not
  automatically all output coverage.
- Recording queue staging and background incorporation are still separate
  phases. A queued Tick write does not become a random-access read until the
  appropriate immutable published data root contains it.
- Reclamation respects pins and the authoritative versioned data. Cached
  realizations may be discarded; unreconstructible recorded information may not.
- Virtual mapping and physical page aggregation are optional implementation
  techniques, not the authored semantics. They do not justify whole-Region
  computation or defeat explicit resource limits. Tick recordings and Tock
  buffered data share the background-managed storage contract and may choose
  the same sparse page-backed realization, subject to configurable per-view
  and aggregate virtual-address budgets in §8.

## 8. Shared buffered storage, sparse mappings, and resource limits

**All `buffered` outputs use one background-managed buffer-realization
contract**, regardless of whether Tick or Tock supplies their values. A
Tick/Sequential/buffered recorder and a Tock/Region/buffered producer differ
in *how data arrive*, not in the storage capabilities presented to downstream
consumers. Recording writes pass through preprovisioned realtime-to-background
queues; Tock values arrive through background evaluation. After incorporation,
both update the same kind of versioned, partially materialized buffer resource.
Do not build a second recording-only allocation/paging/publication engine
beside a Tock-buffered engine. The existing `PersistedDataStore` page directory,
`memfd_create()` backing, and immutable recording snapshots are implementation
precedents to reuse or generalize, **not** a requirement that every buffered
output use Linux mappings.

The shared contract keeps distinct:

- full logical Region identity, `IndexCoverage`, and recording's addressed
  overwrites/`write_void()` erasures;
- the physical backing directory, page/window allocation, and per-version
  *materialized-valid* positions;
- the selected readable mappings/windows and their snapshot pins; and
- authoritative source contents versus disposable copies or computed caches.

A backing page can straddle a Region boundary, a covered/uncovered boundary,
or an overwrite/erasure boundary. Allocated memory, a readable zero-filled
mapping, and a valid result are **not equivalent**. Publication must track exact
validity and coverage separately; page-aligned allocation cannot invent
coverage or expose unfinished content. The executor may round *storage and
work batches* to pages when the producer can correctly calculate the covered
positions of the rounded window. Reverse dependencies remain algorithm-specific,
not implicitly page-shaped. Event buffers may need chunked/variable-capacity
backing instead of sample-page mapping; they still use the same ownership,
versioning, demand, and compatibility rules.

### 8.1 Sparse contiguous views are an executor-selected realization

For an eligible, finite-sized buffered Region, the background executor may
reserve one contiguous virtual address interval and map physical pages at
their final offsets as demanded ranges become valid. On Linux this may use
`mmap()` with sparse `memfd` backing and page-aligned remaps. This allows later
requests to fill additional portions **without copying or regenerating
previously completed unchanged pages merely to extend the buffer**. A sparse
mapping reserves address space; it does not by itself calculate values,
ensure RAM residency, or make an uncovered/uncomputed page readable as valid
data. A Region can remain partially materialized even when one virtual span
has been reserved for its entire finite byte extent.

A selected immutable snapshot must not see page contents change underneath its
readers. Work may populate private, unpublished candidate pages; changed
pages in a successor version acquire distinct physical backing where required,
while unchanged pages may be shared. Published virtual views and backing pages
remain alive for their pins. No in-place `MAP_SHARED` overwrite may mutate a
page visible through an older snapshot. Page reuse, remapping, compaction and
cache eviction happen in the background after the relevant ownership checks.

This mechanism also applies to **recorded** buffers: queued Tick writes and
explicit erasures update the shared versioned page directory, while untouched
ranges retain their prior data. The same buffer allocator and mapping strategy
may be selected for background-produced Tock/buffered results. An existing
recording backing store is authoritative data, however, and cannot be evicted
without retaining its information elsewhere; a regenerable Tock result may be
recomputed. This difference is a property of the data's provenance, **not** a
third authored output realization or a different page-store interface.

**Reconstruction policy (owner correction, 2026-11-17):** Sequential/buffered
recording data remain non-recomputable. Region/Coverage buffered results are
recomputable for their selected snapshot and have cache priority over computed
results. Page and view ownership remain separate; pinned readers keep their
contents alive. See [reconstruction and cache priority](#reconstruction-and-cache-priority-owner-decision-2026-11-17).
The earlier preserve-all-valid-pages policy is retained
[historically](./historical/buffered_reconstruction_before_2026-11-17/output_capabilities_and_background_demand.md).

### 8.2 Bounded virtual address reservations

Sparse contiguous mapping is **optional, not universal**. The background
executor owns resource configuration, including at least:

- a configurable maximum byte extent for **one contiguous virtual buffered
  Region view** (`max_contiguous_buffer_mapping_bytes`, conceptual name); and
- a configurable aggregate budget for **simultaneously reserved virtual
  buffered Region views** (`max_total_buffer_mapping_bytes`, conceptual name),
  including pinned older versions and transition candidates.

The concrete configuration names, conservative defaults and platform-specific
limits are to be chosen during implementation; no fixed constant is part of
the authored port schema. The manager must check range-to-byte overflow,
address-space limits, page alignment, mapping count/fragmentation, and physical
backing/capacity budgets **before** reserving or expanding a mapping. The per-
view maximum bounds virtual-address exposure; the aggregate budget prevents
many individually acceptable views from exhausting the process address space.
These limits do not narrow a port's logical `GlobalIndex` range, alter its
coverage or silently truncate a Region. Virtual-byte budgets are not a
proxy for physical memory use or kernel mapping metadata; §8.3 defines
additional independent budgets.

When a Region exceeds sparse-mapping eligibility, the executor may instead
select bounded windows, segmented/page-directory storage, a rolling Sequential
buffer, or another compatible strategy. The source remains `buffered`: no
particular backing implementation is observable through that authored enum.
A windowed or Sequential consumer still needs only its selected prepared
ranges. A **complete-buffer** input is different: the executor must supply one
actually contiguous, completely valid and pinned Region within the chosen
resource limits. It must not substitute segmented/windowed storage and present
it as a full pointer. If no eligible implementation can meet that demand, report
a clear resource/graph-preparation failure (with required byte extent and
applicable limit), or require an explicit bounded materialization node; never
attempt an unbounded mapping or silently broaden the demand.

### 8.3 VMA fragmentation and independent resource accounting

Sparse backing is page-granular, but Linux virtual memory areas (VMAs) are
**not** necessarily page-granular. Reserving a logical Region can begin with
one virtual interval, and overlaying physically consecutive backing-page
runs can cover many pages per `mmap()` call/VMA. The existing recording
`RecordingPageRunNode` index and `map_recording_region()` run iteration in
`src/intravenous/execution/storage/persisted_data_store.cpp` demonstrate that
implementation direction. Do not replace run-aware mapping with an
unconditional `mmap()` per page. Conversely, scattered replacements and
shared immutable snapshot mappings can fragment a previously compact view
into many VMAs. A small virtual-byte reservation may be more expensive than
a larger but well-coalesced mapping.

The background executor must manage **independent resource budgets** for:

- per-view reserved virtual bytes and aggregate live reserved virtual bytes,
  counting candidates, pinned readers and older compatible snapshots (§8.2);
- mapping fragmentation, including a process-wide VMA-count/headroom budget
  and the number of backing runs or mapping operations needed to assemble a
  proposed view; unrelated process mappings also consume the Linux
  `vm.max_map_count` allowance;
- physical backing consumption and reclaimable backing capacity, including
  copy-on-write versions and pages retained by reader pins; and
- prepared resident/pinned realtime-readable pages and the corresponding
  system memory/locking limits.

The VMA budget is **not** a maximum page count and must not be calculated by
assuming one VMA per page. Count or conservatively estimate mapping effects
using the actual run layout, observe process-wide usage with safety margin,
and perform probing/accounting on the background or control path only.
Actual kernel limits and available address space vary by host and process.
If fragmentation exceeds the safe budget, the executor may coalesce or
rebuild unpinned views, select another compatible representation, defer work,
or fail the preparation explicitly. It must never modify a pinned reader's
mapping or downgrade a complete-buffer input to segmented access.

The initial per-view, aggregate, VMA and physical-backing defaults are
**measurement decisions** rather than authored graph semantics. For example,
4 GiB per view and 64 GiB aggregate are possible *provisional test settings*,
not established safe defaults or universally available Linux limits. Measure
real process mapping counts, backing growth, assembly costs and snapshot
multiplicity under the workload described in §8.5 before setting defaults.

### 8.4 Safe backing-page reclamation

The common buffered-storage manager must reclaim or reuse obsolete physical
backing when the last snapshot, mapping, candidate, reader or authoritative
owner referring to it has released that ownership. Mapping lifetime,
validity metadata, physical page ownership and cache eligibility are
separate: unmapping a view does not prove its backing is disposable, and
evicting a recomputable cache must not discard the sole authoritative copy
of a recording. An old `MAP_SHARED` reader must never observe recycled,
truncated, overwritten or hole-punched backing. Reclamation, hole punching,
free-offset reuse and compaction, if selected, occur off the audio thread
only after all relevant pins and committed-version references are accounted
for. Preserve unchanged physical pages across immutable versions.

The current `RecordingPageAllocator::allocate()` in
`src/intravenous/execution/storage/persisted_data_store.cpp` grows its `memfd` and
advances `next_file_offset_` monotonically; it does **not** yet reclaim
obsolete backing offsets. This is a source-state observation, not normative
permission for unbounded growth. Generalizing that allocator for Tick
recordings and Tock/buffered outputs must include bounded-accounting and
safe reclamation/reuse so repeated overwrites of a fixed logical recording
range do not grow physical backing indefinitely after old snapshots retire.
If backing cannot be freed or retained information cannot be moved safely,
report resource pressure rather than silently losing a selected data version.

### 8.5 Realtime boundary and implementation requirements

The background executor selects or replaces storage implementations, commits
pages, assembles mappings, resolves missing content, and prepares/pins upcoming
realtime views. Realtime Tick callbacks perform **none** of those operations.
A page that is valid in the page directory may still fault when accessed, so
realtime preparation must also establish the required residency/availability
under the system's locking and memory budgets. A preparation failure follows
the existing underrun/error contract, not an audio-thread page-fault-driven
request. Realtime recording writes use preprovisioned capacity, then hand off
their blocks for background incorporation; they do not create virtual mappings
or allocate backing pages during `tick()`.

Validate this shared realization with sparse nonadjacent requests, later fills
of the same Region, overlap and partial-page boundaries, overwrite/void and
untouched recording behavior, immutable old-version reads, pinned mappings,
cache eviction/reconstruction, request-order independence, virtual-reservation
budget exhaustion, and complete-buffer demand that exceeds its limit. Also
test dense contiguous backing against heavily fragmented overwritten runs,
repeated writes into a bounded recording range followed by snapshot retirement
and backing reuse, partial-page copy-on-write, unrelated process mappings,
simultaneously pinned old and candidate versions, VMA budget exhaustion, and
physical-backing or realtime-residency pressure. Track reserved bytes,
physical backing bytes, live/reclaimable pages, mapping/run counts and
mapping-assembly work separately. Compare measured latency/fragmentation
rather than inferring them from the number of logical pages. Source
implementation may select different backends for different buffer sizes or
pressure levels without adding node-visible storage configuration.

## 9. Migration obligations

The pre-migration implementation used `TickOutputConfig`,
`OutputRetention::{ephemeral,persisted}`, replay/coverage coupling, strict
persisted retention, whole-Region materialization and complete-coverage Tock
publication assumptions. Their replacement and verification status are recorded
in the [implementation plan](./automation_and_background_migration_plan.md).
These historical assumptions **must not** be mistaken for the destination above. Migrate all relevant
static schemas, reflection/serialization, GraphJit connection analysis,
replay/SCC analysis, output recording and versions, background demand/realization,
callback bindings, and public tests together; do not leave compatibility
aliases merely to keep an intermediate build passing. Generalize recording's
page-backed versioned representation into shared buffered-output realization,
with background-owned backend selection and bounded virtual reservations,
rather than implementing a separate Tock-only storage path (§8).

Prior intent and its original wording are preserved in
[the historical snapshot](./historical/output_ports_before_computed_buffered_2026-11-16.md).
