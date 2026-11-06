# Outstanding Implementation Fronts

_Status: coordination map for design work that is compatible with the current direction but has not yet fully landed in the implementation._

This document groups the remaining work by implementation ownership rather than by
which design document first introduced it. The goal is to make the outstanding work
parallelizable: one person or team should be able to own one front, define the missing
local details inside that boundary, and integrate through explicit contracts with the
other fronts.

This is not a replacement for the detailed design documents. Those documents remain
normative for semantics. This document answers a different question: **where should
implementation ownership be cut?**

A front may contain a mixture of completely unimplemented work and partially landed
infrastructure. "Outstanding" means the front is not complete enough to remove from
the implementation roadmap.

## Coordination principles

The fronts below follow five rules:

1. **Separate semantic contracts from storage representations.** Port realization,
   random-access storage, capture, and revision migration interact, but they should not
   be one implementation project.
2. **Keep authored callback shape behind node traits.** GraphJit should consume
   normalized operations and solved realizations rather than rediscovering source-level
   callback choices.
3. **Keep project intent distinct from derived runtime state.** Configuration source,
   connections, semantic node events, and persistence belong to the control plane;
   GraphJit and executor runtime artifacts do not.
4. **Land correctness before optimization.** Revision continuity, transactionality,
   and semantic delivery must not depend on a later batching/fusion/PGO pass.
5. **Depend on completed explicit integration contracts.** If a neighboring front's
   current representation cannot support the feature, record that mismatch as
   implementation friction and change the dependency order or design. Do not bridge an
   incomplete prerequisite with a temporary adapter merely to keep another front
   moving. The superseded policy is retained in
   [historical documentation](./historical/implementation_fronts_temporary_adapter_policy.md).

## Front 1: port constraints and realization

**Primary ownership:** `constrain_ports()`, graph-wide port-property solving, and the
immutable resolved realization consumed by declaration, storage planning, scheduling,
and lowering.

This front owns:

- `ConstrainPortsContext<Node>`;
- dynamic continuous-value `size()` constraints;
- exact `pace()` constraints and pace-domain solving;
- local `pace() = n` ratios and explicit `pace().absolute() = n` graph-time anchors,
  both restricted to power-of-two factors, with one canonical project-rate scale for
  each wholly unanchored connected pace component;
- weighted dyadic pace relations in which each port's authored power-of-two factor is
  relative to its owning node's logical step, connections equate absolute base-2 pace
  exponents, and inconsistent exponent sums around cycles are rejected;
- Sequential-input `history()` constraints;
- Tick-output `latency()` constraints;
- Coverage ordering/sortedness constraints;
- finite registered value-type alternatives and value-type equality constraints;
- explicit audio-capability constraints contributed by graph tiling, applied to every
  tiled continuous port without affecting event ports;
- connection-generated constraints between producer and consumer ports;
- conflict and unresolved-property diagnostics;
- the shared Q64.64 `FractionalGlobalIndex` node-visible execution and transported-
  coordinate domain, the initial supported absolute-exponent range `[-64, +64]`, and
  realization diagnostics for graphs outside that range;
- compiler-owned physical Sequential cursors per used dyadic pace, preserving
  invocation-order storage/history across discontinuous global transport seams without
  exposing another node time coordinate;
- one unchanged project/execution sample rate, constrained port pace as the transported-
  value cadence, and type-owned per-value sample support; distinct physical sample-rate
  domains and fractional resampling are deferred;
- per-port Tick block extents;
- Sequential `block_extended()` realization;
- the initial continuous-value registry, including `Sample`, `FractionalGlobalIndex` and
  `FFTBlock`;
- declaration-generated continuous-value identity/metadata/capability domains, with
  value-type-specific storage and authored-access behavior added by the type extension
  rather than by editing central consumer switches;
- the canonical `FFTBin = std::complex<Sample::storage>` representation and the
  preliminary real-transform `FFTBlock` layout of `N / 2 + 1` explicit complex bins,
  including DC and Nyquist;
- FFT-node realization requirements for backend layout, alignment, padding,
  in-place/destructive operation, scratch storage, canonicalization and normalization,
  so direct library use is selected only when compatible and every other path is
  preplanned rather than allocated dynamically during execution;
- the backend ownership boundary: core transports `FFTBlock`, while an explicitly
  selected transform-node IV package alone includes and links its FFT implementation;
- realization-time selection of one concrete type for multi-type ports before storage
  planning/lowering;
- graph-resolved FFT dimensions and storage-size information;
- the planned type-safe continuous/event port-config representation, with authored
  `input<Types...>()` / `output<Types...>()` declarations in heterogeneous tuples,
  scoped typed access/production tokens, and scoped output-retention values;
- value-type-aware module/subgraph boundary input declarations, without imposing
  Sample-only default/min/max semantics on `FractionalGlobalIndex` or `FFTBlock`;
- extension-provided defaults and per-Sequential-input overrides for
  `neutral_value` / `combine_values` where their reduction algebra affects
  realization and delivery legality.

Coverage ordering is a solved graph property, not only a static producer flag. A node
may make the ordering guarantee of one output depend on the ordering guarantee of an
input:

```cpp
void MergeLikeNode::constrain_ports(
    ConstrainPortsContext<MergeLikeNode>& ctx) const
{
    ctx.output<"out">().ordering() = ctx.input<"in">().ordering();
}
```

Connections then propagate the required/guaranteed ordering relationship through the
graph. A downstream sorted-Coverage requirement can therefore become a transitive
constraint over the nodes feeding it. The solver may satisfy that requirement through
naturally sorted producers and k-way descriptor merging, or require an explicit
ordering realization where an upstream contribution is not guaranteed sorted. Exact
constraint-proxy spelling is owned by this front; the semantic requirement is not.

The output contract of this front is an immutable **resolved port realization**. Other
fronts should consume that object instead of each independently resolving size, pace,
history, latency, or ordering.

**Can start independently:** yes. The parser, metadata, value model, stable reference
representation, and scope-local resolver can land without Front 10 or Front 12. The
canonical node-ID spelling must agree with Front 10. If a typed reference needs a live
handle across `BuilderSession` objects, that materialization step depends on Front 12.

**Important consumers:** fronts 2, 3, 4, 6, 7, and 8.

Detailed direction:
[Graph JIT Direction](./graph_jit_direction.md),
[Random-Access Port Data Direction](./random_access_port_data_direction.md), and
[Sequential Port Storage And Connection Planning](./sequential_port_storage_planning.md).

Active source migration status:
[Port Realization Migration Status](./port_realization_migration_status.md).

**Current implementation status:** the working tree now implements the Front 1
constraint solve, type-owned continuous geometry, per-port dyadic Tick projection,
physical Sequential cursor realization, bounded `block_extended()` materialization,
producer-shared detached-revision startup validity, and concrete node-facing
Tick/Sequential access for `Sample`, `FractionalGlobalIndex`, and `FFTBlock`. The latest
end-to-end runtime tests await repository-owner validation. Generalized Tick/persisted capture is
owned by Front 5, Region/Coverage realization is owned by Front 3, and live realized
introspection is owned by the ProjectGraph/runtime read-model join; those downstream
features do not keep the Front 1 execution contract open.

## Front 2: batched primitive callbacks and scheduling

**Primary ownership:** first-class scalar/batched node callback normalization and the
scheduler's ability to exploit same-implementation batches.

**Implementation status (2026-10-23):** complete and verified. The collaborator's
full suite passed all 592 tests after Front 4 supplied the background-only delivery
required by the final replay and reverse-propagation fixtures. Per-lane contiguous
scratch (friction item 2) is deferred to Front 16 as an optimization. The
implementation includes the
public lane ranges, exclusive authored scalar/batch forms, every normalized helper
and compiler anchor, exact implementation-plus-resolved-realization classification,
dependency-safe `SchedulePlan::ready_groups`, conservative realtime batch lowering,
and dynamic active-lane background batches for authored forward, reverse, and Tock
callbacks. The initial scheduler caps one emitted batch at eight lanes. It keeps a
ready group scalar when primitive slicing, persisted capture, or producer-side storage
operations would require extra control flow or longer transient live ranges. TTL
activity no longer forces scalar fallback. `emit_execution_batch()` evaluates
every lane's activity decision first. It then compacts the ticking lanes and the
dormant lanes into opposite ends of one `ReflectedNodeTickBatchLane` array. It calls
`tick_block_batch()` and `skip_block_batch()` on the two spans, and each call is
guarded by a nonzero lane count. Afterward it finalizes activity state for the lanes
that ticked. A group with no activity-enabled lane emits neither the decision nor the
skip call. It also compares callback-facing physical ranges from
`SampleTransientAllocationPlan`, `SamplePersistentAllocationPlan`, and
`EventTransientAllocationPlan`. A group stays scalar if a writable range overlaps
another lane or if a lane uses the shared contiguous-input scratch area.
Measurement-driven sizing and joint storage/layout costing remain Front 16 work, as
intended.

Focused source tests cover scalar/batch trait exclusivity, singleton adaptation,
sample-major/lane-minor pointwise order, separate reflected port bindings per lane,
dependency-preserving ready groups, aliased-storage scalar fallback, and generated
Tick, skip, forward, reverse, and Tock batch calls. The generated-root fixtures use
two instances with different configuration bytes. Both instances must observe a
two-lane batch. This enforces the rule that configuration values do not belong to the
compatibility key. A generated replay fixture also executes a batch-only Tick source
through the normalized one-lane scalar adapter.
`ActivityPartitionsReadyFrontierIntoTickAndSkipBatches` covers one ready group with
two different TTLs. The group splits into one-lane Tick and skip batches, and later
both lanes skip in one two-lane batch.

**Verification follow-up (2026-10-23):** the collaborator's first full run failed 141
of 584 tests. Every heavy package build stopped in `iv_package_finalize` with
"malformed iv_node_types operations ABI". Front 2 had added five batch anchors to
`NodeCompilerOperations`, which now has 13 function pointers. `scan_node_records()`
in `tools/iv_package_finalize.cpp` still required 8 operands. The check now
requires 13. A collaborator rerun remains pending.

**Second verification run (2026-10-23):** 581 of 585 tests passed. The remaining
failures expose three unresolved friction signals. These are observations, not
accepted design:

1. *Ready-frontier ordering versus transient storage.* The ready-group loop in
   `connection_plan.cpp` selects the largest same-class ready set first. In
   `reused_sample_arena_module`, the flattened order changed from
   `source_a, sink_a, source_b, sink_b` to `source_a, source_b, sink_a, sink_b`.
   Both producer buffers are then live at once and exceed the stack budget, so they
   move to `NodeStorage`. `StackBudgetUsesPackedSampleLifetimes` now sees three raw
   regions instead of one. The reordering happens before lowering decides whether
   the group actually batches. This front owns "scalar fallback when batching makes
   the surrounding schedule worse", so the reordering is a Front 2 regression.
2. *Shared contiguous-input scratch excludes most block callbacks.*
   `requires_contiguous_workspace` assigns the shared scratch area to every authored
   `tick_block()` or `tick_block_batch()` input. `try_emit_execution_batch()` keeps
   every such lane scalar. Native realtime batching therefore applies only to
   primitives without continuous inputs or to zero-history pointwise `tick()`
   primitives.
3. *Connected background fixtures depend on missing Front 4 delivery.*
   `NativeBackgroundCallbacksReceiveReadyFrontierBatch` and
   `BatchOnlyTickCallbackExecutesThroughReplayScalarAdapter` connect sample ports
   that only background evaluation reads. Sample-port planning in
   `lowering_plan.cpp` rejects every `SampleProducerGroupPlan` without realtime
   connections ("does not yet support background-only sample connections"). That is
   Front 4's "background-only sample delivery". The re-audit below says Front 4 needs
   only a closure audit, and this rejection contradicts it.

**Friction decisions (2026-10-23; collaborator rerun pending):**

1. Accepted: planning owns the batch decision. `realtime_ready_group_batches()` in
   `lowering_plan.cpp` now holds the checks that `try_emit_execution_batch()` used
   to make during LLVM emission: batch symbols, slicing, producer-side storage
   operations, captures, callback-storage overlap, and contiguous scratch.
   `build_lowering_plan()` removes every member of an ineligible multi-lane group
   from its realtime batch classes. It then plans again until every remaining
   multi-step `ExecutionReadyGroupPlan` batches. `emit_execution_batch()` emits
   those groups without re-deciding. If the grouped schedule exceeds the stack
   budget, the planner first returns to the scalar Kahn order and only then applies
   stack pressure. Batching therefore never causes a move to `NodeStorage`. Over
   budget, this rule gives up all realtime batching. Weighing batching against
   `NodeStorage` moves belongs to the Front 16 cost model. The background plan keeps
   the full batch classes, because later analyses reuse the retained background
   plan.
2. Resolved as Front 16 optimization work: batching does not add copies.
   `prepare_contiguous_inputs()` in `node/build_request.h` copies into
   `contiguous_scratch` only when the requested window wraps the source ring.
   Scalar and batched execution behave alike there. A batch only needs each lane to
   own a disjoint wrap-fallback region, because every lane facade is live until the
   batch callback returns. That costs reserved stack bytes, not work. Part of the
   cost comes from the representation: scratch is reserved for every block-access
   input, even one whose window provably cannot wrap. The remainder is the real
   trade-off of stack bytes against batching, which the Front 16 cost model must
   account for. The current behaviour preserves every lane's data for the lifetime
   it needs, and no code depends on how the scratch is partitioned. Choosing
   per-lane scratch is therefore an optimization only, and is deferred to Front 16
   until measurements justify it.
3. Accepted: `NativeBackgroundCallbacksReceiveReadyFrontierBatch` now uses two
   input-less `NativeBatchTockProbe` instances and covers forward propagation and
   Tock evaluation. Reverse propagation needs a Random Access input. A disconnected
   one has no background storage, so `validate_background_runtime_plan()` rejects
   its empty runtime binding; that is Front 4's disconnected/default binding work.
   `NativeReversePropagationReceivesReadyFrontierBatch` therefore keeps Codex's
   original connected module for the reverse batch.
   `BatchOnlyTickCallbackExecutesThroughReplayScalarAdapter` cannot avoid a
   background-only Tick-to-coverage connection and is kept unchanged. At that
   verification point, both were expected to fail until Front 4 supplied the
   missing bindings.

**Third verification run (2026-10-23):** apart from the expected Front 4 failures,
`TransitionRootPreservesRewiredHistoryAndMirrorsNewProducerSamples` failed. The
transition builder in `lowering.cpp` copies the steady successor's `LoweringPlan`
and then attaches transition storage operations to individual primitive steps,
after planning has already decided which ready groups batch.
`emit_execution_batch()` skipped those per-step operations. Before the planner
owned the decision, the emitter's own re-check had kept such groups scalar. The
transition builder now calls `split_unbatched_ready_groups()`. That function
re-applies `realtime_ready_group_batches()` and splits each group that no longer
batches into singleton groups in the same order, so execution positions stay
unchanged.

**Fourth verification run (2026-10-23):** 584 of 586 tests passed. Only
`NativeReversePropagationReceivesReadyFrontierBatch` and
`BatchOnlyTickCallbackExecutesThroughReplayScalarAdapter` failed. Both stop at
"GraphJit realtime lowering does not yet support background-only sample
connections", which is the expected Front 4 boundary. They should pass once Front 4
delivers background-only samples.

**Completion verification (2026-10-23):** the working-tree Front 4 changes removed
the background-only sample guard. The collaborator's complete 592-test suite passed.
This verifies both previously blocked fixtures and closes Front 2. The earlier
verification records above remain as the defect history that led to the final
scheduler and lowering shape.

This front owns:

- authored `tick()`, `tick_block()`, and `tick_block_batch()` normalization;
- authored `skip_block()` and `skip_block_batch()` normalization;
- authored `tock_coverage()` and `tock_coverage_batch()` normalization;
- forward/reverse propagation scalar and batch normalization;
- iterable batch contexts with `size()`, `operator[]`, `begin()`, and `end()`;
- singleton-batch adaptation when a node only defines a batch callback;
- sample-major/lane-minor generated batching for `tick()`;
- ordinary lane-loop generated batching for scalar block callbacks;
- compiler/reflection anchors for every normalized scalar and batch operation;
- batch-compatibility keys;
- dependency-aware grouping of ready nodes from the same batch class;
- virtual-node pre-grouping as an acceleration hint rather than a semantic boundary;
- scalar fallback when batching makes the surrounding schedule worse;
- batched background propagation and Tock ready-frontier execution.

The node-trait API is the only place that branches on which authored callback exists.
GraphJit sees `do_*` and `do_*_batch` operations unconditionally.

`same_callback_batch_class()` compares the concrete implementation and every
callback-facing entry in `ResolvedPortRealization`. It excludes configured values,
`State`, `TockState`, coverage values, and storage addresses.

**Can start independently:** yes.

Detailed direction:
[Batched Node Callback Direction](./batched_node_callbacks_direction.md).

## Front 3: Region/Coverage random-access representation

**Primary ownership:** the authored and runtime random-access data model after the
canonical retained-data checkpoint.

This front owns:

- first-class contiguous `Region` values;
- `Coverage` as a collection of possibly overlapping Regions;
- preservation of multiplicity/overlap rather than forced disjoint normalization;
- unordered and sorted Coverage representations;
- canonical sorted order and k-way descriptor merging;
- Region/Coverage Tock inputs and outputs;
- Region/Coverage Random Access inputs;
- bounded Coverage-to-Sequential reduction;
- ephemeral materialization ownership;
- persisted Region/Coverage publication and versioning; and
- integration of canonical persisted sample regions with the authored/runtime
  Region/Coverage model.

This front consumes solved ordering requirements from front 1. It owns **how** sorted
Coverage is represented or materialized, not **whether** a particular graph edge
requires sortedness.

**Can start mostly independently:** yes. Canonical persisted storage already exposes
explicit sample regions/event chunks; use that final store boundary directly and do
not introduce an adapter to the deleted page representation.

Detailed direction:
[Random-Access Port Data Direction](./random_access_port_data_direction.md) and
[Coverage And Background Evaluation](./coverage_and_background_evaluation.md).

### Front 3 entry friction: logical Region replacement (2026-10-24)

The planned data-bearing `Coverage` preserves overlapping Regions, duplicate Regions,
and Region decomposition. At Front 3 entry, the runtime could not preserve those
facts. It reduced
`RegionInputConfig` and `CoverageInputConfig` to
`BackgroundPortPlan::random_access_input`. It also reduced `RegionOutputConfig` and
`CoverageOutputConfig` to `BackgroundPortPlan::authored_tock_output`.

`TockSampleOutputPort` then wrote one `Sample` for each global index and channel into
storage selected by one positional `IndexCoverage`. `TickMaterializedSampleInput`
explicitly stored disjoint records. `PersistedDataStore::Candidate::overwrite()`
removed every sample record that intersected the new range before it inserted one
replacement `PersistedSampleRegion`. `BackgroundStorageRealization::stage_persisted_data()`
also rejected overlapping storage slots for one persisted output. These paths
flattened all Region decomposition and multiplicity.

Changing the range tree to permit overlaps is not sufficient. A partial recomputation
can replace the middle of one logical Region. Splitting that Region into physical
prefix, replacement, and suffix records changes the Region sequence observed by a
Coverage callback. The result can then depend on transaction request boundaries even
when the values do not change. Conversely, preserving one logical Region requires a
rule that identifies which old Region each new Region replaces, or requires the
runtime to recompute and publish a complete logical Region set.

Front 3 had to settle one of these contracts before source implementation continued:

1. Region decomposition is semantic. Add logical Region identity and publish a batch
   replacement operation that preserves overlaps and duplicates independently of the
   store's physical range-tree entries.
2. Region decomposition is semantic, but any intersecting change recomputes and
   replaces a complete logical Region set. This needs a rule that widens positional
   demand and invalidation to those complete Regions.
3. Lossless splitting of one Region into adjacent Regions is semantically equivalent.
   This weakens the current statement that a consumer receives and can process each
   Region independently. It is a normative design change, not an implementation fact.

The selected direction combines the first two contracts without adding authored
stable Region IDs. Keep `IndexCoverage` as the positional set used by propagation.
Store each logical Region as one immutable object, and make physical index entries
refer to that object. Before reverse planning, expand a replacement domain to contain
every old logical Region that it intersects. The callback then publishes the complete
replacement Region batch for that domain. Every new Region must fit inside the domain,
or the node underreported its changed coverage. The node-facing callback ABI must
publish Region descriptors and contiguous backing as a batch instead of writing one
sample at a time.

### Front 3 Region-output allocation decision (2026-10-25)

A `CoverageOutputConfig` continuous output exposes a pre-allocated list of writable
Region outputs to its `tock_coverage()` callback. Forward propagation determines the
complete ordered list of logical `GlobalIndexRange` values, including overlaps and
duplicates. The positional union remains available as `IndexCoverage` for change and
demand propagation, but it is derived information and cannot replace the Region list.

The existing transaction order supplies the allocation boundary. First,
`BackgroundPropagationWorkspace::prepare()` completes forward propagation and reverse
propagation. Next, `BackgroundEvaluationTransaction` selects the required storage and
constructs `BackgroundStorageRealization`. Generated background evaluation runs only
after that construction. `BackgroundStorageRealization` can therefore allocate the
selected Region descriptors and their contiguous backing before `tock_coverage()` is
called. The callback fills those fixed buffers; it does not append Regions, resize a
Region, or allocate output backing.

Every selected Region receives an independent writable storage range. Logical overlap
between two Regions never aliases their values. An implementation can suballocate
several Regions into disjoint byte ranges of one owner allocation, but it cannot reuse
the same bytes for their overlapping logical positions.

For a `RegionOutputConfig` continuous output, the same representation has exactly one
Region. For a `CoverageOutputConfig` continuous output, the list can contain zero or
more Regions. The propagation result must retain the list shape because the former
`OutputCoverageChange::publish_coverage()` path retained only `IndexCoverage`, which
normalized away overlap, duplicates, ordering, and Region count.

This contract also means that output Region boundaries cannot first be discovered by
examining sample values during `tock_coverage()`. A node whose Region list depends on
data values must determine and retain that list before forward propagation, or use a
different authored representation. An allocator callback inside `tock_coverage()` and
node-owned output buffers are rejected: both move output ownership and allocation into
the evaluation callback even though the propagation phase already exists to describe
the output shape.

### Front 3 source status (2026-10-26)

The collaborator compiled the continuous completion candidate and the subsequent
event/API revision and reported that all 641 tests passed. The next audit correction
adds effective read latency to stable input-connection migration, rejects
Tick/persisted recordings as Region sources, makes event ordering explicitly
per-Region, and extends the generated callback test through continuous and event
Region outputs. These corrections and their adversarial tests await the next
collaborator run.

`ContinuousRegionStorageView` is the non-owning callback ABI. Node code receives it
through `ContinuousRegion<Value>`, `WritableContinuousRegion<Value>`,
`ContinuousCoverage<Value>`, and `WritableContinuousCoverage<Value>`. The resolved
`ContinuousValueStorageGeometry` supports `Sample`, `FractionalGlobalIndex`, and
`FFTBlock`; the Region contract is not limited to scalar samples.

`OutputCoverageChange::publish_regions()` now publishes the complete logical Region
list. `BackgroundCoverageState` and `PreparedCoveragePropagation` preserve that list.
They derive `IndexCoverage` for positional change and demand algebra. Sorted Coverage
uses the canonical begin-ascending, equal-begin extent-descending order. Fan-in merges
sorted Region runs with a k-way merge. Stable sorting preserves producer order for
exact duplicate Regions. Channel assembly forms one intersection for each Region
tuple needed to construct one value. Separate
`SampleProjectionPlan` contributions retain separate Region lists, so fan-in does not
turn disjoint contributions into an empty intersection or merge overlapping
contributions into one Region.

The typed `PropagateForwardCoverageContext` facade now exposes one public operation:
`publish(GlobalIndexRange)` for a Region output and
`publish(span<GlobalIndexRange const>)` for a Coverage output. The static output
configuration selects the valid overload. The rule is identical for continuous and
event outputs. The superseded event-only positional-publication design is retained in
[historical documentation](./historical/front_3_event_coverage_before_anonymous_region_lists_2026-10-26.md).

Event Region/Coverage ports now preserve the same anonymous ordered Region list as
continuous ports. `EventRegion` and `WritableEventRegion` expose one independent event
sequence. `EventCoverage` and `WritableEventCoverage` expose an iterable list of those
Regions. Overlapping and duplicate ranges do not share event storage. Event fan-in
clips each source Region to the common available positional coverage, then retains one
Region contribution per source. Sequential consumers receive a premerged immutable
timeline; the audio thread does not allocate or sort while reading it.

A Tick/persisted recording does not preserve authored Region membership. Its
positional overwrites produce canonical Regions from `IndexCoverage`; overlapping and
touching recorded ranges merge. It therefore satisfies Coverage inputs but not Region
inputs. Replayed Tick/ephemeral execution remains Region-shaped for each replay
invocation.

The recording storage conflict is resolved. `PersistedDataStore::Candidate::overwrite()`
does not retain one `PersistedSampleRegion` allocation per captured block. For example,
captures `[0, 4)` and `[4, 8)` update sparse recording pages, and their canonical
`IndexCoverage` publishes one `ContinuousRegionStorageView` for `[0, 8)`.

The preliminary correction is a sparse page-backed recording representation owned by
`PersistedDataStore`. It replaces the earlier segmented-public-Region proposal, which
is retained in
[historical documentation](./historical/front_3_segmented_recording_region_proposal_before_sparse_pages_2026-11-01.md).
This is now the implemented recording representation.

`IndexCoverage` remains the only semantic record of positions that contain valid
recorded values. A separate sparse physical page directory maps page-aligned timeline
positions to backing pages. The page directory must not define coverage because one
page can contain both covered and uncovered positions. The implementation must split
ranges at the signed-order seam and must reserve bounded virtual-address windows; it
cannot map the complete cyclic `GlobalIndex` domain.

Authored Tock Region/Coverage publication and Tick recording require different
`SampleOutputState` representations. `replace_sample_regions()` retains independent
authored Regions, including overlaps and exact duplicates. A recording-specific
candidate operation writes a realtime-produced Tick payload directly into affected
recording pages and updates `IndexCoverage`. It does not create one
`PersistedSampleRegion` per captured block. The realtime Tick callback still copies its
output into the preallocated `ProducedBlockChain`; the background transaction commits
that owned payload into the recording pages.

On Linux, an anonymous RAM-backed file created with `memfd_create()` supplies the
physical pages. `ftruncate()` grows its physical-page file and `mmap()` places
backing-page runs into a
reserved contiguous virtual interval. One `ContinuousRegionStorageView` then describes
each canonical `IndexCoverage` range. Large uncovered gaps remain separate Coverage
Regions and consume no sample pages. If later writes fill a gap completely, the
canonical ranges merge and a new contiguous virtual view can map the retained pages
without copying the complete recording.

Published `PersistedDataStore::Snapshot` views remain stable. A candidate that
overwrites a page already visible through an older snapshot allocates a new page,
copies only that page, and updates its path-copied page directory. Untouched pages stay
shared. Each snapshot owns its virtual mappings until existing reader pins release it.
The copied sample bytes are therefore bounded by the new payload and affected boundary
pages rather than by the complete recording. A second path-copied
`RecordingPageRunNode` index records logically and physically consecutive pages.
Virtual-view assembly visits those physical runs instead of looking up every page in a
long canonical Region. An ordinary append therefore extends one run, while an
overwrite splits only the run that contains each changed page. Reclaiming or compacting
obsolete backing pages remains Front 16 work. Copying or walking the complete recording
on every captured block remains disallowed. The superseded page-walk classification is
retained in
[historical documentation](./historical/front_3_page_walk_before_physical_run_index_2026-11-01.md).

Persisted Tick continuous outputs use interleaved storage only. Reflection forces an
authored Tick/persisted output to interleaved while preserving its `ChannelTypeId`.
GraphJit rejects a manually assembled or archived configured graph that still marks
such an output planar. Ordinary connection conversion supplies planar storage to a
downstream input that requests it. Authored Tock Region/Coverage outputs can still use
planar storage. The superseded open layout question is retained in
[historical documentation](./historical/front_3_sparse_recording_before_interleaved_only_decision_2026-11-01.md).

Persisted Tick event outputs use the same Coverage rule but do not need continuous
virtual storage. Their canonical Regions can retain chunked event records because
`EventRegion::for_each()` already visits events through the port operation. Persisted
Tick sample and event outputs remain invalid Region-input sources.

The suspected replayable-Tick-to-Region scheduling conflict was an invalid authoring
example, not remaining scheduler work. The example declared a Coverage output while
its `tock_coverage()` implementation required exactly one output Region. Such an
output must use `OutputProduction::region`.

`BackgroundPropagationWorkspace::require_output()` widens any partial demand on a
non-Tick output to each complete intersected authored Region before reverse
propagation. A Region-to-Region pass-through therefore requires one complete input
Region from its replayable Tick source. A node with a real Coverage output can still
use a Region input when reverse propagation maps all selected output Regions to one
contiguous `GlobalIndexRange`. If it requires several disjoint input Regions, the
input must use `InputAccess::coverage`.

`BackgroundEvaluationCallFrame::seal()` remains the defensive validation that a
Region input reaches evaluation with exactly one Region. Front 3 does not need a
per-Region Tock invocation exception or a transaction split. The false analysis is
retained in
[historical documentation](./historical/front_3_replayable_tick_region_multi_invocation_false_start_2026-11-01.md).

`BackgroundStorageRealization` allocates one independent contiguous buffer for each
selected logical Region before `tock_coverage()` runs. All produced
`PortStoragePlan` roles for one `BackgroundPortIndex` output share the same
`TransactionSampleStorage` or `TransactionEventStorage` owner. This gives the node
callback one writable Region list even when planning retains both `background` and
`persisted_data` roles. It also prevents a callback from writing duplicate logical
Regions merely because the output has several physical consumers.

An externally bound `BackgroundSampleReadView` or `BackgroundSampleWriteView` must
match the planned `ContinuousValueStorageGeometry` and the complete `ChannelLayout`.
Every selected position must also have a matching `ContinuousRegionStorageView`.
`BackgroundStorageRealization::seal()` rejects a view whose summary metadata or Region
descriptors disagree with the plan.

`build_background_storage_plan()` keeps a direct sample route for a Random Access
input only when the source already supplies the input port's complete channel layout.
If `SampleConnectionPlan::requires_conversion` is true, the plan creates a
`SampleMaterializationPlan` whose output uses the target layout. This includes channel
tiling, channel permutation, and channel conversion. It prevents a Coverage callback
from receiving complete per-channel coverage facts with no complete target-layout
Region view.

`populate_background_topology()` also rejects a `RegionInputConfig` when its normalized
continuous connection contains more than one `SampleProjectionPlan`. Each projection
is one logical contribution, so such a connection can supply Coverage but cannot
supply the input's required single Region.

`PersistedDataStore::Candidate::replace_sample_regions()` and
`replace_event_regions()` replace complete logical Region batches produced by
authored Region/Coverage callbacks.
`PersistedSampleRegion` owns registered continuous-value bytes, while
`PersistedDataStore::Snapshot::sample_region_views()` exposes matching
`ContinuousRegionStorageView` records. `PersistedEventChunk` owns one event Region.
Overlapping Regions, exact duplicate Regions, and producer order remain visible after
publication for both port kinds. A replacement domain cannot clip an old logical
Region.

`TickMaterializedSampleInput` retains the logical Region list and resolved geometry.
`TickMaterializedEventInput` retains the event Region list and a background-built
merged timeline for Sequential playback. Tick Random Access views bind the Regions
directly. Coverage-to-Sequential
materialization and `TickInvocationWorkspace::SequentialSampleSlot` use the target
input's generated neutral initializer and combiner. This applies to resolved
`FFTBlock` values as well as `Sample` values.

Reverse propagation closes a required authored Region/Coverage output over complete
logical Regions before it calls the node's reverse callback. This rule applies to
event outputs as well as continuous outputs. A narrow request that intersects one
event Region therefore cannot ask the node to compute part of that Region while
storage selection prepares the complete Region list. Replayed Tick outputs are not
included: their `IndexCoverage` is positional and their invocation schedule remains
bounded by the requested positions.

Tick-materialization carry-forward uses an explicit current materialized-record order.
For the current Sequential Tick-input path, the transaction builds that order from
the newly produced positional request windows and the wholly retained previous
windows, then applies signed-canonical ordering. `carry_forward()` no longer infers a
replacement position from the previous snapshot alone. Its lower-level merge also
preserves an explicitly supplied nonadjacent order such as `[A, C, B]`. The same rule
applies to `TickMaterializedSampleRegion` and `TickMaterializedEventChunk`.

Coverage-state migration now requires matching `OutputProductionKind` for both event
and continuous outputs. A stable event port that changes from Coverage production to
Region production does not inherit a predecessor Region list that violates its new
shape contract. External `BackgroundEventReadView` and `BackgroundEventWriteView`
bindings must also supply Region descriptors and the matching Region callback when
their coverage is nonempty.

Compiler-generated contextual replay remains positional. Its output ports have
`OutputProductionKind::tick`, so `replay_forward_coverage()` calls the internal
`OutputCoverageChange::publish_coverage()` operation. Only authored Region and
Coverage propagation uses the Region-list operation exposed through the typed
`publish(...)` facade. Persisted replay results use
`PersistedDataStore::Candidate::overwrite()` for each selected positional record;
they do not use `replace_sample_regions()` or `replace_event_regions()`, which enforce
the authored logical Region-list replacement contract.

Generation reconciliation uses the same `BackgroundConnectionPlan` route and
`SampleProjectionPlan` logic as ordinary propagation. An
`InputCoverageChangeRequest` created by `BackgroundCoverageState::Migration::commit()`
therefore carries both the current positional coverage and the current logical Region
list. Rewiring a stable continuous or event input does not reduce its Region shape to
`IndexCoverage` during migration.

Reuse of `TickMaterializedSampleInput` and `TickMaterializedEventInput` also follows
whole-Region ownership.
`BackgroundEvaluationTransaction::restrict_input_demand()` retains a previous
`TickMaterializedSampleRegion` only when changed coverage does not intersect it. Any
current demand that intersects such an unchanged Region retains the complete Region.
If changed coverage intersects it, storage planning recomputes the Region instead of
clipping and carrying a positional remainder.

The added adversarial tests cover empty Coverage output and empty Coverage view
iteration, invalid Region cardinality,
rejection of positional publication for a continuous Tock output, Region-input source
validation, overlap and exact-duplicate preservation, independent overlapping
storage, signed-order sorted fan-in, complete logical replacement, producer-home
storage sharing,
target-layout materialization for a stereo tile, preservation of a direct route for
an exact stereo source, separate normalized projection contributions, partial-channel
materialization completeness, static continuous/event publication separation,
rejection of multiple projection contributions at a Region input, stable ordering of
exact duplicate Regions, rejection of external continuous views whose full layout or
Region descriptors disagree with the plan, stable-input migration with a logical
Region list, whole-Region Tick materialization carry-forward, resolved `FFTBlock` Region callbacks,
persisted `FFTBlock` Regions, Tick Random Access materialization, and overlapping
`FFTBlock` reduction into a Sequential Tick input, event Coverage propagation through
the generated callback ABI, independent overlapping event Regions, persisted event
Region replacement, and deterministic Coverage-to-Sequential event merging with
equal-time duplicates preserved. The generated callback test also applies a narrow
demand to the first of two overlapping Regions and verifies that the producer still
receives both complete output Regions required by the positional replacement domain.
Light storage and Tick-materialization tests also cross the signed-order seam and
verify forward order relative to the event Region's own `begin`. Additional tests
keep a narrow Tick replay request bounded inside a much larger available coverage,
partially overwrite persisted Tick sample and event records without erasing their
unselected prefix or suffix, preserve an explicitly supplied
`[recomputed A, retained C, recomputed B]` carry-forward order, reject event coverage
migration across production kinds, and reject nonempty external event views without
a matching Region list and Region reader. Event fan-in coverage also verifies that
each source Region is clipped to the common complete range without merging exact
duplicate contributions, and that Random Access event materialization maps those
duplicate clipped ranges back to the correct source Regions.

The audit tests additionally verify that effective sample read-latency changes create
an exact stable-input reconciliation root, Tick/persisted recording coverage derives
maximal contiguous Regions from `IndexCoverage`, Tick/persisted sample and event
outputs are rejected as Region-input sources, event writes are ordered independently
inside each Region, and compiled sample/event Region-output leaves pass one Region
through an `IV_MODULE` to authored Region-input leaves.

The event Regions are intentionally anonymous. Their list positions are meaningful
within the current publication and callback but are not stable identities. Optional
explicit identifiers, dynamic identifier creation, UI enumeration, and selection by
identifier remain future work rather than Front 3 completion requirements.

A separate limitation remains outside Front 3. Background contextual replay of a Sequential
continuous input still accepts only `Sample` at project rate. The validation is in
`connection_plan.cpp` and belongs to Front 7 replay generalization. It does not limit
authored Tock Region/Coverage production, persistence, Tick Random Access, or ordinary
background-to-Sequential delivery.

## Front 4: complete background-to-Tick delivery

**Primary ownership:** execute every already-planned ordinary background delivery at
Tick time.

This front owns:

- sequential persisted/materialized snapshot playback into Tick inputs;
- neutral values for genuinely absent Sequential data;
- background-only sample delivery;
- mixed Tick/background sample fanout;
- background-only event delivery;
- mixed Tick/background event delivery;
- disconnected/default behavior for those dynamic bindings;
- removal of the current capability guards that reject those cases.

This front does not own capture, project persistence, or general graph-revision state
migration. It must expose the background playback window that Front 6 needs when a
Sequential input changes delivery kind.

**Implementation status (2026-10-23):** background planning, storage,
transactions, materialized playback, neutral fill, and generated-root
`TickInvocationCall` binding exist. Realtime planning accepts background-only
Sequential sample and event inputs, which `PrimitiveSampleInputBindingPlan` and
`PrimitiveEventInputBindingPlan` mark as `invocation_supplied`. Mixed Tick/background
composition is implemented for samples (`combines_invocation_supplied` and the node's
`combine_sequential_input_values`). It is also implemented for events
(`iv_graph_jit_merge_mixed_event_sequences`, with semantic source indices carried
through background materialization). Disconnected Sequential event inputs share
compiler-emitted immutable empty sequences.
`GraphExecutor::set_tick_materialization_demand()` accepts an exact `IndexCoverage`
of future Tick positions; its caller chooses the extent.

**Source closure pass implemented and verified (2026-10-23):**

- *Done in source:* Random Access inputs on Tick primitives are view-only.
  `StaticRandomAccessSampleInputTickAccess` and
  `StaticRandomAccessEventInputTickAccess` expose only the callback-pinned view,
  as published-snapshot-only Tick-time Random Access requires. The binding plans mark
  such inputs `random_access`; their declared reflected Sequential entries are never
  read. A primitive with a Random Access input does not enter TTL dormancy.
  A disconnected Random Access input has empty coverage. Its binding has no storage,
  which `is_disconnected_random_access_input()` admits, and
  `binding_selection()` selects nothing for it. Mixed event delivery is now decided
  per target, so a connection that also feeds a Random Access target no longer
  makes its realtime Sequential targets mixed.
- *Done in source:* `TickMaterializationSnapshot` inputs are ordered immutable
  `TickMaterializedSampleRegion` records and `TickMaterializedEventChunk` records
  over shared storage, not one flat vector per port. Playback copies whole region
  spans without allocating, and `at()` binary-searches. A transaction restricts each
  Tick Sequential playback demand through
  `BackgroundCoverageDemandExpansion::restrict_input`. It removes the coverage that
  the same generation's published snapshot still represents: that coverage minus
  the forward-changed coverage of the routed source outputs, shifted by read latency.
  `TickMaterializationSnapshot::carry_forward()` then shares those retained records
  in the new snapshot. A transaction triggered by recorded data therefore
  recomputes only invalidated playback coverage.
- *Superseded:* the playback-slot cross-kind transition source and the mixed-input
  limitation recorded at closure are retained in [Background-fed input playback
  history](./historical/background_fed_input_playback_history_2026-10-23.md). The
  reopened item below replaces them.
- *Verification result:* the collaborator's complete 592-test suite passed. The two
  final failures were stale fixtures: the mixed Tick/Tock event-fan-in test still
  expected both sources to be materialized, and a manually constructed
  `EventMaterializationPlan` omitted its required `semantic_source_indices`. Their
  corrected expectations passed with the complete suite. Front 4 is complete for its
  background-to-Tick delivery scope.

**Status:** complete and verified. The separate mixed-input transition follow-up is
owned by Front 6, not by this delivery front.

**Post-closure review (2026-10-23; correction compiled, full suite passing):**
- *Defect found and corrected in source:* the steady mixed event merge passed the
  callback `sample_index` as the ordering origin to
  `iv_graph_jit_merge_mixed_event_sequences`, which compared
  `origin.forward_distance_to(time)`. Every history event of either source therefore
  sorted after every current-block event of the other, so a mixed input with
  history saw an out-of-order `EventInputPort` sequence. The realtime operand
  (a producer-group canonical sequence or a derived materialization) and the
  `TickInvocationCall` slot retain different horizons, and the derived
  materialization's horizon can still grow after `configure_mixed_input` runs. The
  helper now orders by `GlobalIndex::signed_distance_to` from the callback position,
  which is exact for bounded Sequential horizons and needs no planned origin.
- *Verification gap:* the 592-test run did not exercise generated execution of a
  Tick primitive whose Sequential input is supplied through `TickInvocationCall`
  slots. "Verified" above covers the planned shapes and runtime helpers only. The
  original wording is retained in [Background-fed input playback
  history](./historical/background_fed_input_playback_history_2026-10-23.md).
- *Documentation retention:* the status text replaced at closure is retained in
  [Front 4 status before closure](./historical/front_4_status_before_closure_2026-10-23.md).

**Reopened, implemented and verified: node-owned history for background-fed inputs
(2026-10-23; the collaborator's full suite, including the end-to-end fixtures below,
passed without warnings).** A Tick primitive's Sequential input fed
through a `TickInvocationCall` slot now keeps the history it consumed, in Tick
invocation order, with neutral history before its first invocation, as [GraphJit
direction](./graph_jit_direction.md) requires for every Sequential input. The
superseded representation and why it was wrong are retained in [Background-fed input
playback history](./historical/background_fed_input_playback_history_2026-10-23.md).

- *Timeline:* a slot-fed input with nonzero history, and every mixed input, binds
  `PrimitiveSampleInputBindingPlan::channels` or `PrimitiveEventInputBindingPlan::storage`
  to its own retained timeline in `NodeStorage`. The callback reads that timeline
  like any other retained input.
  - *Samples:* `SampleInputTimelineRequest` is planned by the same retained-window
    path as a disconnected Tick output, as a ring or compact carry chosen by the cost
    model. Its persistent frames are initialized to the input's neutral value, and it
    is addressed by the generation pace cursor.
  - *Events:* a persistent ring with the ordinary seek rebase and history pruning. A
    primitive sliced in a cyclic region prunes once before the region.
- *Per-callback write:* before the callback, the root writes only the current block
  into the timeline (`stage_tick_sequential_sample_timelines`,
  `stage_tick_sequential_event_timelines`).
  - *Pure input:* the slot window.
  - *Mixed sample input:* the node's `combine_sequential_input_values` applied to
    the realtime operand (`realtime_operand_channels`) and the slot window, one
    block at a time.
  - *Mixed event input:* the current window of the existing deterministic merge of
    `realtime_operand_storage` with the slot window. The merge orders by signed
    displacement from the callback position, as recorded above.
- *Zero history:* a slot-fed input without history still binds its slot directly.
  The generated root stores the slice offset as its ordinal, so an SCC slice reads
  its own part of the window.
- *Workspace:* `TickInvocationWorkspace` slots hold only the root window
  `[P, P + block)` at frames `[0, block)`, with ordinal zero. Tick materialization
  demand (`set_tick_materialization_demand`) now covers playback positions only.
- *Consequence for Front 6:* these inputs are ordinary `direct` retained state, so
  graph revisions in both delivery directions use the existing ring/compact-carry
  transfers and transition suffixes. The playback-slot transfer source and the
  Tick-to-Tock staging transition root were removed with the superseded
  representation.
- *End-to-end fixtures (verified):* compiled GraphJit roots driven
  through a real `TickInvocationWorkspace` and `TickInvocationFrame`, with each
  callback's playback published as a `TickMaterializationSnapshot`.
  - Pure sample and pure event inputs keep neutral pre-start history. They keep
    consumed values when earlier positions are republished, and keep
    invocation-order history across a seek; event history moves with the
    transport.
  - A slot-fed input without history reads its window at a position that is not
    a multiple of the slot capacity.
  - A mixed stereo sample input and a mixed event input keep their combined
    history.
  - One graph revision exchanges the sources of two consumers, so one moves from
    realtime to background delivery and the other from background to realtime,
    for samples and for events. Each keeps the history it consumed before
    cutover.
- *Status:* complete and verified. Front 4 is closed again.

Detailed direction:
[Coverage And Background Evaluation](./coverage_and_background_evaluation.md).

## Front 5: realtime-produced queues and explicit recording

**Primary ownership:** realtime-safe production of persisted/background-visible data
from realtime execution and its ownership transfer to background work.

This front owns:

- producer-specific `ProducerReserve` / background `PendingQueue` transports backed
  by power-of-two blocks;
- `AsyncCapacityManager` provisioning/reclamation with producer `C/L/H` reserve policy
  and manager-owned allocation granularity;
- one released-block return stream from background to capacity management where
  practical;
- direct sample/event production into provisioned blocks where layout permits;
- cheap complete-chain publication at realtime pass boundaries;
- Tick-persisted output handoff;
- explicit recorder handoff with fixed untouched/write/`write_void()` semantics;
- independent finite-prefix pinning by `BackgroundExecutor`;
- latest-compatible immutable persisted-state mailbox publication back to realtime;
- publication into the canonical random-access persistence representation; and
- release/reclamation of completed prefixes after successful domain commit.

There is intentionally no atomic snapshot across different producer queues. If a
future semantic requirement needs one, stop and design that requirement explicitly.

Integrate with the Region/Coverage target of front 3 rather than reintroducing an
adapter to the deleted fixed-width page representation.

**Dependencies:** front 4 for the complete consumer side; front 3 is strongly preferred
before the durable publication layer is finalized.

**Current implementation status (2026-11-01):** implementation is complete and
verified. The repository-owner build succeeded, and the full suite passed all 661
tests. `ProducerReserve`,
`PendingQueue`, `AsyncCapacityManager`, per-output capture disposition, finite-prefix
pinning, transaction consumption, canonical persisted publication, generation-aware
mailbox publication, and reclamation were already present. The final correctness gap
is now closed: persisted Tick continuous outputs carry the resolved
`ContinuousValueStorageGeometry` and absolute pace into each compiled capture slot.
`TickInvocationWorkspace` projects each root invocation range onto that port's value
lattice, copies arbitrary registered continuous values from the physical
invocation-order ring, and publishes value-lattice ordinal ranges. The background
transaction validates the exact `ChannelLayout` and
`ContinuousValueStorageGeometry`, then commits the payload through
`PersistedContinuousStorage`. `write_void()` uses the same projected range. Persisted
Tick continuous outputs remain interleaved at the capture boundary; ordinary planned
conversion supplies planar consumers.

This closes Front 5 correctness only. Bulk ring capture, streaming queued block chains
directly into persisted storage, and related wake/capacity reductions remain Front 16
performance work. They are not required to represent or publish a retained continuous
value correctly.

## Front 6: graph-revision state continuity

**Primary ownership:** preserving semantically valid node/port state when one compiled
graph generation is replaced by another.

This front owns:

- stable matching of concrete semantic nodes and ports across revisions;
- Sequential history continuity;
- Tick-output latency/revision-window continuity;
- rewiring and fan-in/fanout changes;
- value-size and pace changes after front 1 lands;
- temporary transition realizations when old and new storage cannot be represented by
  one steady-state layout;
- transition expiry horizons and switch to the final realization;
- compatible persisted random-access rebinding;
- one complete realtime/background `ExecutionGeneration` identity and stable route/destination identity;
- producer/destination disappearance semantics for queued recording/persisted data;
- ordered old-generation drain + prepared background migration across realtime cutover;
- generation-compatible persisted-state publication; and
- generation reconciliation and stale-generation rejection/reclamation rules.

This is correctness work, not an optimization pass.

**Dependencies:** current executor infrastructure; consumes front 1's solved
realization and front 3's final persistence representation as those land.

**Current implementation status (2026-10-18):** cold semantic inventories, stable
sample/event port-state identities, old/new interval matching, physical realization
descriptors, direct steady-storage transfer plans, and executor cutover integration are
present. The implementation classifies inherited state as already represented,
transferable to private steady storage, requiring transition-only storage, or
unavailable from the predecessor. Direct project-rate sample ring/compact-carry and
event-ring transfers execute at the cutover position. Their descriptors and storage
extents are now validated and owned by a prepared cutover program before publication;
the realtime transfer routine neither allocates nor has a recoverable error path.
Unsupported transition-only or unavailable inherited state is rejected during cold
staging rather than being silently omitted at cutover. Materialized/composed sample
state, converted/indexed event state, non-project-rate continuous state, transition
suffix lowering, and the transition expiry path remain incomplete. Activation as a
whole can still fail through ordinary `NodeStorage` lifecycle callbacks, so the final
cutover boundary is not yet completely non-failing.

**Active implementation friction (unresolved; 2026-10-18):** preserving bounded
inherited port state is necessary, but the source does not yet demonstrate that this
requires a separately compiled transition realization plus a separately compiled
steady realization. That two-realization scheme is one mechanism for retaining
steady-state aliasing and eliminating transition costs after inherited state expires;
it is not itself the semantic continuity requirement. The implementation should compare
three forms before expanding `CompiledGraph`/`ExecutionGeneration` ownership:

1. the currently documented transition/steady realization pair;
2. one successor physical realization with bounded transition-only carries or
   materializations and prepared bindings/operations that stop using them at expiry;
3. deliberately private successor port state that can directly receive inherited
   values, accepting permanent steady-state storage/copy cost.

The leading concrete form of option 2 is one successor `NodeLayout` whose ordinary
steady-state regions form a stable prefix and whose bounded transition-only regions
form a suffix in the same `NodeStorage` allocation. GraphJit can emit a transition
root and a steady root against the same offsets and authored `State`; expiry then
switches root entrypoints without another node-state migration. Transition suffix
regions must be compile-time bounded, initialized before activation, excluded from
ordinary persistent raw-region migration, and exposed as semantic sources if another
revision arrives before expiry.

This form has two explicit costs which must not be hidden. First, current nested-node
state spans previously extended from a child state to `NodeLayout::storage_size`.
`NodeLayout` now records `steady_storage_size`, transition-only raw declarations form
a checked suffix, and nested spans stop at the steady boundary. Second, one ordinary
contiguous `NodeStorage` allocation cannot release only its suffix. Expiry can remove
all access and cache traffic, but the reserved bytes remain until the execution
generation retires. Measure the maximum
tail size before deciding whether that bounded retained capacity is acceptable or
whether later off-thread reclamation/segmented ownership is justified.

**New implementation friction (unresolved; 2026-10-18):** the current compilation
request contains only the new logical graph. Reconciliation is first planned later by
`ExecutionGeneration`, after the successor `NodeLayout` has been finalized and both
realtime and background `NodeStorage` objects have been allocated. Transition suffix
shape and transition-root bindings therefore cannot be added truthfully at that late
stage. In addition, `BackgroundGeneration` currently creates storage from the same
layout, so a suffix appended without an allocation-extent distinction would be paid
twice even though the background root never addresses it. Before suffix lowering, pass
the active predecessor's semantic inventories into compilation (without giving the
compiler mutable executor ownership) and let background storage allocate only the
steady prefix. This moves transition planning ahead of final layout/emission while
retaining one canonical offset model.

**Partial resolution and further friction (2026-10-18):** `NodeStorage` now owns an
explicit allocation extent. Realtime generations allocate the complete canonical
layout, while background generations allocate and expose only `steady_storage_size`;
transition-only raw initializers and region views are excluded from that shorter
instance. This resolves the duplicated-background-tail part of the preceding friction.

**Ordered transition coordination decision (2026-11-01):** `ProjectGraph` owns the
ordered executable revision request. `GraphExecutor` accepts only a successor whose
named predecessor is its accepted chain tail. Once it accepts A to B, it must preserve
and perform A to B before a later accepted B to C transition. It must not replace the
pending generation, silently skip the intermediate transition, or return a normal
retry result because realtime activation moved while compilation ran. Failure before
acceptance leaves the accepted tail unchanged. Failure during an accepted cutover is
fatal.

Application-produced cutovers name the accepted predecessor's steady realization. If
that predecessor is still in its transition realization, the next accepted cutover
waits in the ordered chain until steady state is active. The executor does not accept a
cutover whose only valid predecessor phase can expire before activation.

`ProjectGraph` remains the parent of both `GraphJit` and `GraphExecutor` and enters
each once per rebuild. The superseded replaceable-pending context and phase-retry
design is preserved in
[`historical/front_8_replaceable_pending_transition_false_start_2026-11-01.md`](historical/front_8_replaceable_pending_transition_false_start_2026-11-01.md).

**Front 8 completion (2026-11-02):**
`ProjectGraph` now sends one `ProjectGraphGraphJitRequest` and, after successful
compilation, one `ProjectGraphGraphExecutorRequestedEvent`. `GraphExecutor::stage()`
accepts only the exact ordered chain tail. It retains every accepted successor and
publishes one successor at each realtime pass boundary. First-generation background
ownership is installed during cold staging; the audio boundary publishes it without
taking `BackgroundExecutor::commands_mutex_`. The executor request result separately
records whether the singleton bridge was bound, so an unbound bridge cannot be
mistaken for acceptance. A stale submission from `ProjectGraph` is an invariant error.

`SystemAudioDevices` now supplies `ResourceContext::SystemAudioResources`. The
registered `iv.builtin.system_audio_input` and `iv.builtin.system_audio_output` leaf
nodes resolve stable logical bindings in `initialize()`. An unavailable input reads as
silence and an unavailable output discards writes. If no selected hardware output can
open, an internal project-rate fallback output clock keeps the graph running. An
available hardware output replaces that fallback through the normal stop-and-join
device boundary. The output callback slices device requests into project blocks, calls
`GraphExecutor::activate_pending()`, and then calls `GraphExecutor::tick_block()`.
Output writes for the selected logical device mix into the current hardware slice.
Input capture passes through
`AudioInputSynchronizer`; its realtime consumer uses `try_lock()` and emits a silent
slice instead of waiting for the capture thread.

The server constructs `GraphExecutor` with both system-audio and JUCE VST resources,
stops audio threads before destroying the executor, and monitors the executor's sticky
production, capacity, and background failures. A detected failure requests application
shutdown and is returned as a server error. Source-level tests cover ordered
multi-generation acceptance, migration through an already accepted intermediate
generation, the complete `ProjectGraph -> GraphJit -> GraphExecutor` chain, output-clock
activation, slicing a large device request into project blocks, logical-output mixing,
fallback-clock execution, stable binding activation after device selection, and
missing-device silence/discard. The repository-owner build succeeded, and the complete
667-test suite passed. Front 8 is complete at this boundary. Automatic graph-node
creation for detected devices remains the optional convenience behavior stated below,
not unfinished Front 8 work.

The layout half of that specialization boundary is also present.
`NodeLayoutTransitionBuilder` accepts an already-finalized steady layout, preserves all
steady offsets, and permits only transition raw regions to be appended. The
sample-storage layer can now realize the representative project-rate direct-ring case
whose successor steady target is shared: it appends a private suffix target and extends
the already prepared cutover transfer so inherited samples are copied there without
allocation at activation. Event conversion, materialized/composed samples, and
non-project-rate samples remain explicit unsupported cases. The next connection is for
GraphJit to consume the staging context's predecessor inventories, bind the affected
authored port to that suffix target in a transition root, and emit it together with the
steady root. The specialized result should augment the steady successor analysis; it
must not replace `ProjectGraph`'s predecessor-independent desired/diagnostic artifact.

The comparison must preserve allocation-free, non-failing realtime activation;
replacement while a transition is still active; static exclusion of transition work
from unaffected graphs and ports; and no accidental permanent copy/memory cost merely
to simplify migration. Until a hostile rewiring/fan-in case has been implemented end to
end, do not treat the current one-`CompiledGraph` limitation as proof that the runtime
must grow a two-realization container. This review does not change Front 6's semantic
scope or its position as the next execution-correctness front.

**Representative transition-root path implemented (2026-10-18; verification
pending):** GraphJit can cold-specialize an already compiled steady successor from
the exact predecessor supplied by `ProjectGraph`.
For feed-forward project-rate `Sample` input history backed by a canonical persistent
ring, specialization appends private suffix rings, transfers inherited history at
cutover, mirrors newly produced samples into those rings while the transition root is
active, and emits transition and steady entrypoints in the same LLVM module. Both roots
share the steady layout prefix and authored node `State`; expiry changes only the
selected entrypoint. `CompiledGraph` retains the prepared semantic cutover and the
transition-phase realization inventory, while its weak predecessor token avoids pinning
obsolete generated code for the successor's lifetime.

`ProjectGraph` always compiles from the accepted predecessor's steady realization.
`GraphExecutor` preserves that accepted cutover until activation. If the predecessor
is still executing its transition root, the successor waits until the predecessor
reaches steady state. Activation selects the successor transition root without
allocation, and callback completion switches that generation to its steady root once
the finite history horizon has elapsed. The contiguous suffix allocation is retained
until the generation retires, as already recorded above.

This representative implementation deliberately rejects output history/latency,
producer-after-consumer feedback order, materialized or composed sample state,
non-project-rate state, and event transition storage. It also reparses and lowers the
successor package LLVM during cold specialization instead of retaining a reusable
`LoweringPlan`; measure that duplicate cold compile cost before adding longer-lived
compiler artifacts. The compiler/executor coordination API is present, but the
application-side owner which invokes it is still part of the later runtime integration
front. Focused generated-root and executor phase/expiry tests have been added; their
build and execution result is intentionally supplied by the human collaborator.

The first full-suite verification compiled successfully and passed 529 of 530 tests.
The sole failure did not reach the transition root: its test constructed concrete
package nodes directly, outside a virtual module embedding, so under the current stable-
identity rules those nodes had no cross-generation identity and specialization had no
semantic states to match. The fixture now uses a registered module graph and rewrites
only a copied connection record, preserving virtual-node identity; verification of that
correction subsequently passed. The full suite therefore passed all 530 tests for the
representative direct-ring transition-root landing.

**Expansion in progress (2026-10-18; verification supplied after the next build):**
prepared sample cutover can now reconstruct a predecessor's retained linear channel
materialization instead of requiring the predecessor semantic channel to be stored
directly. Transition suffix allocation groups disjoint roles/channels of the same
authored port into one physical timeline. This permits Tick sample output history and
latency to share one suffix ring: the transition callback writes that ring, an ordered
post-callback mirror updates the ordinary steady producer representation before fanout
and carry commit, and the steady root resumes from the evolved steady representation.
Feedback timelines also have a late transition-mirror phase after feedback and
composition writes, removing the producer-before-consumer ordering assumption without
changing ordinary materialization order. Composed predecessor state, non-project-rate
state, and event transition suffixes remain incomplete.

**Sequential-cursor correction and pace expansion (2026-10-19; verification
pending):** the preceding project-rate restriction hid an incorrect addressing
assumption in the representative cutover. Continuous rings remain Sequential across a
seek because they use one persistent linear cursor per resolved pace; the cutover had
instead selected ring slots from the node-visible `GlobalIndex`. Every sample
realization now records its generation's pace-cursor offset. Prepared transfer
validation checks that cursor, and realtime reconstruction addresses predecessor and
successor storage from their respective linear ordinals. A transport jump therefore
does not reinterpret history. A pace change preserves the overlapping sequence of
node-owned transported values without implicit resampling, even when the old and new
pace cursors have different physical origins. The project-rate-only gate has been
removed from direct and materialized sample transition storage. Focused coverage now
uses different old/new pace exponents and cursor origins together with an unrelated
cutover `GlobalIndex`; compilation and execution remain for human verification.
Transition expiry now uses the same model. The executor cold-allocates one progress
record per affected successor pace, snapshots each migrated cursor at activation, and
switches to the steady root only after every required transported-value horizon has
actually advanced. A seek cannot prematurely expire the transition, and a slow or fast
port is no longer measured in project-rate samples. The existing before-callback
materialization order also supplies pre-consumer feedback mirrors, closing the remaining
sample feedback-order guard.

**Resolved semantic direction and implementation in progress (2026-10-19):** two
remaining Front 6 boundaries required explicit contracts rather than storage guesses.

- Sequential events retain absolute `TimedEvent` coordinates but must also remain in
  invocation order across a transport seam. The current cutover filters them by a
  `GlobalIndex` window around activation, which necessarily drops pre-seek history.
  The chosen contract separates durable and ephemeral coordinates. Persistence and
  Random Access retain the authored absolute timestamp. Storage retained only for
  Sequential execution instead moves with the transport: it records the expected next
  global position, detects a seek when the next callback begins elsewhere, and rebases
  its bounded retained events by that seek delta before exposing or pruning them. This
  preserves one time-sorted node-facing window without a per-event coordinate sidecar.
  The expected-position marker is analogous to a continuous ring's private cursor, but
  event values must move because `TimedEvent` carries its coordinate.
- A surviving dynamically sized value such as `FFTBlock` can change resolved size.
  The type registry now owns an optional cross-shape migration operation. If the type
  omits it, the successor intentionally starts fresh initialized state; generic graph
  code never byte-prefix-copies a value. `FFTBlock` currently chooses that fresh-state
  default because no bin-resize semantics have been authored. A future type can supply
  a realtime-safe operation without adding a type switch to transition planning.

Composed predecessor sample history remains ordinary implementation work once these
semantic boundaries are settled: its retained contribution graph must be carried into
the cold transition descriptor and evaluated into the suffix. It does not resolve or
avoid either issue above.

The first human verification of the Sequential-cursor/output-window expansion compiled
and passed 532 of 533 tests. The sole failure did not enter output-window lowering: the
fixture discarded its authored producer expression, so package source annotation gave
that otherwise executable disconnected node no stable virtual identity and transition
matching found no surviving port state. The fixture now names the producer declaration
and asserts that both output-window requirements have stable identities before testing
the transition root; verification of that correction is pending.

**Boundary implementation update (2026-10-19; human verification pending):** the
contracts above are now represented in the transition program rather than remaining
design notes. Persistent Sequential event rings and compact carries own an initialized
expected-next-position marker. Restore/prune rebases only their live bounded events
when the visible transport position jumps, and prepared cutover rebases the predecessor
sequence to the activation position before interval selection. Event transition expiry
advances by completed callback extents and is independent of visible `GlobalIndex`
differences.

Prepared event transfer now accepts both ring and compact-carry predecessors and
successors, applies a planned type conversion when one exists, and merges multiple
semantic transfers into one target without resetting earlier contributions. Indexed
fan-in targets retain the current producer index beside every copied event, including
deterministic equal-time ordering. A semantic event state that cannot receive a direct
steady-storage transfer gets a private transition ring. Front 7 now uses this rule for
both derived Sequential inputs and a raw event output that shares its steady sequence
with an input. The input ring receives current delivery before its consumer executes.
The output ring receives the callback-local raw sequence after its producer executes.
The earlier output-suffix assumption is preserved in
[`historical/front_6_event_output_transition_suffix_assumption.md`](historical/front_6_event_output_transition_suffix_assumption.md).

The continuous side now carries flattened retained composition sources and evaluates
their nonzero channel coefficients during prepared reconstruction. Same-type dynamic
shape changes call the optional registry-owned migration operation; absent support is
an explicit fresh-state result. These changes close the previously listed composed
predecessor, non-project-rate addressing, dynamic-shape, and ephemeral-event transition
items at source level. Focused coverage was added for compact/indexed event cutover,
seek rebasing, composed reconstruction, fresh dynamic shapes, and seek-independent
event expiry. No compilation or test execution was performed by the implementation
agent, per the collaboration agreement.

**Generation-handoff completion update (2026-10-19; human verification pending):**
the background half now migrates compatible committed output coverage by stable output
identity as a prepared operation. Preparation records only the old/new accumulator
mapping; commit reads the predecessor after its closed queues have fully drained, so it
cannot snapshot coverage too early. Exact sample-layout compatibility or event-type
compatibility is required. Persisted sample destinations additionally require the same
complete channel set. Generation-local outputs and incompatible stable outputs do not
carry coverage or retained records into the successor.

The same prepared handoff identifies persisted destinations which do not survive in a
compatible successor realization. Their store-local handles remain unambiguous, but the
successor reconciliation transaction removes their retained records only after final
old-generation writes have committed. A disappeared producer with a surviving stable
destination therefore preserves its final data, while a disappeared destination is
retired in the documented order. After background storage and coverage migration, one
successor transaction is required even when its new queues are empty. That transaction
publishes a generation-compatible persisted-data/materialization pair, making
post-cutover predecessor-drain results visible to the active realtime generation without
ever publishing a late old-generation root to it. Focused tests cover late-read coverage
migration and transactional destination retirement; compilation and execution remain
with the human collaborator.

Transition-only event storage is always a persistent ring even when the authored
callback capacity is not a power of two. Its cold realization rounds the requested
capacity up to the next valid ring capacity and validates every persistent ring
descriptor before transfer. This keeps arbitrary legal callback capacities out of the
ring-indexing invariant without changing their authored limit.

Destination retirement is ordered before successor writes inside the reconciliation
candidate. This matters when an incompatible realization reuses the same stable output
handle: inherited records are removed first, then new-generation records may establish
the replacement content in the same atomic publication. A successor with no background
plan can perform this retirement-only transaction without generated no-op roots; it
also promotes an empty generation-compatible Tick-materialization snapshot. The active
transition/steady phase token is now atomic because cold staging can inspect it while
realtime callback completion advances it.

**Front 6 friction recorded before resolution (2026-10-19):** stable output identity and physical
storage compatibility are not sufficient to decide whether persisted coverage and data
remain semantically valid. The compiled background plan currently has no stable
computation-revision or dependency-closure fingerprint. If a Tock node's code,
configuration, or upstream computation changes while its output identity and layout
stay the same, the present handoff would preserve coverage which may now be stale.
Invalidating every stable output on every graph compilation would also be wrong: it
would conflate recompilation with semantic invalidation, discard unaffected retained
work, and can violate recording preservation.

Do not guess this equivalence from generation numbers, lowered indices, function
pointers, or raw configuration bytes containing relocations. There are two coherent
ways to supply the missing fact. The application mutation path can carry an explicit
stable semantic-change set from `NodeInstances`, `GraphConnections`, and definition
refresh through compilation/staging; GraphJit can then map those causes to the new
plan's local-node and connection invalidation roots. Alternatively, compilation can
derive a stable computation identity whose dependency closure classifies unchanged
outputs and produces those roots. The former follows the external-invalidation-root
ownership already specified in `coverage_and_background_evaluation.md`; the latter
makes generation reconciliation self-describing but first needs canonical identities
for configuration and provider semantics. Neither contract is present in the current
plans, so choosing its owner is the remaining design boundary requiring collaborator
direction.

**Resolution implemented (2026-10-19; human verification pending):** reconciliation
is self-contained without a transitive computation fingerprint. Each compiled
background node now carries its stable concrete-node identity, registered primitive
identity, exact provider-definition version, and an ownership token for its typed
configured storage. `NodeInstances` preserves that token across definition-snapshot
publication only when the configured instance's complete recursive definition closure
still has the same versions. A rebuilt definition therefore invalidates every cached
parent and every placed instance which uses it, while an unrelated definition refresh
does not discard reusable configurations. The comparison also includes the compiled
local background execution mode and semantic port shape, so a replay/Tock-role or
resolved-port change cannot hide behind an otherwise reusable configuration token.

The prepared handoff compares those local facts and normalized stable-input connection
descriptions against the exact active predecessor. A changed local node becomes a
`locally_changed_nodes` root. A rewire with unchanged local semantics becomes an exact
input-change root whose changed coverage is the union of the predecessor input coverage
and successor input coverage after final predecessor coverage has been migrated. The
ordinary forward coverage pass then carries either kind of root through all downstream
dependents; no transitive hash is stored or recomputed. Sample-rate changes root every
background node because the rate is callback-visible. Initial project loading has no
predecessor and continues to construct fresh state. Stable outputs remain eligible for
coverage/data reuse only until these roots invalidate their affected ranges.

Focused tests now cover unchanged-versus-changed local keys, sample-rate-wide
invalidation, exact stable-input rewiring, preservation of cache entries across an
unrelated snapshot publication, and recursive invalidation when a nested definition
changes. The implementation agent did not compile or execute them, per the collaboration
agreement. This resolves the ownership choice and the last semantic-invalidation
boundary recorded above; any verification failure should be recorded as new
implementation friction rather than worked around with generation-wide invalidation.

**Remaining Front 6 lifecycle friction (2026-10-19):** the complete realtime cutover
still is not non-failing. `NodeStorage::Migration::commit()` invokes authored
`move()`, `release()`, and deferred `initialize()` callbacks after the quiescent
activation boundary. Their erased function-pointer types are not `noexcept`, and the
current API permits `initialize()` to allocate background `TockState`. Catching an
exception is not a recovery policy: raw regions and earlier nodes may already have
moved, earlier predecessor nodes may already have been released, and neither storage
then represents a rollback-safe generation.

Do not paper over this with a catch/terminate block or describe the present callbacks
as realtime-safe by convention. Completing the front requires an explicit lifecycle
contract. The leading direction is to separate background/Tock lifecycle preparation
from the pass-boundary operation and make the remaining realtime `State` transfer a
statically non-throwing commit; this agrees with the intended background-sidecar
ownership of `TockState`. An alternative would need a genuinely transactional
prepare/commit lifecycle API with rollback-safe authored state. Choosing that API is
the next boundary requiring collaborator direction.

**Lifecycle failure policy selected and implemented (2026-10-21; human verification
pending):** the collaborator selected fail-fast process termination instead of recovery.
Authored `initialize()`, `move()`, and `release()` remain free to use allocators and to
throw arbitrary exceptions internally. Their type-erased callback boundaries are now
`noexcept`: each boundary catches only on the exceptional path, records the lifecycle
operation, concrete C++ node type, and node index, and enters `std::terminate()` while
the authored exception remains active. The server's installed terminate handler then
records that exception and a stacktrace, flushes the diagnostic stream, and aborts the
process. The VS Code launcher supplies an `IV_LOG_FILE` path for every server process,
prints that path in the Intravenous output channel, and preserves an explicit
user-provided path. Crash-handler installation opens the selected diagnostic stream
before lifecycle work, so a later allocator failure does not first discover the file.

There is deliberately no `std::expected` result. No caller can safely continue or roll
back after a partially executed lifecycle handoff, so a returned status would add a
success-path check without providing a valid decision. On the ordinary path the erased
callbacks call the authored operation directly; exception reporting, symbolization,
file I/O, and process termination occur only after an exception. The irreversible
`NodeStorage::Migration::commit()` and its executor cutover caller are correspondingly
`noexcept`. This supersedes the preceding unresolved choice with an explicit crash-only
contract; it does not claim that authored allocation has become realtime-safe.

**Front 6 source status (2026-10-21):** with the lifecycle failure policy above, no
known source-level correctness boundary remains in this front. The implemented path now
covers stable semantic matching, continuous and event history reconstruction, pace and
shape changes, transition suffix/root expiry, replacement during an active transition,
background drain and coverage migration, persisted-destination reconciliation,
semantic invalidation, and a fatal non-returning cutover failure contract. This is a
completion candidate, not a verification claim: the collaborator still owns compiling
and running the new lifecycle checks and the full suite. A failure should reopen Front 6
as concrete implementation friction; a clean result closes the front and moves active
implementation to Front 7.

**First completion-candidate verification (2026-10-21; re-verification pending):**
the collaborator's 548-test run passed 542 tests and exposed six failures. Two were
real transition defects. Event history and future roles merged into one target were
ordered around the cutover position independently, which put the future event before
the history events; cold preparation now records one earliest ordering origin for the
shared target, leaving the realtime merge allocation-free and constant-metadata-driven.
Also, retained Tick sample outputs incorrectly reused the callback-facing positive
latency offset even though compact carry is already indexed in semantic output
coordinates. This double shift made the retained future window unrepresentable; the
authored and retained views now carry their distinct offsets.

The other four failures were verification-fixture drift rather than runtime behavior:
materialized sample state can now transfer directly into compatible private steady
storage, semantic coverage comparison requires stable port identities as well as stable
node identities, and two persisted-region fixtures declared four-sample ranges with
only two samples of payload. Their expectations and inputs now state those contracts.
Front 6 remains open until the collaborator reruns the affected and full suites.

**Second completion-candidate verification (2026-10-21; re-verification pending):**
the next 548-test run passed 546 tests. The shared-event ordering, materialized steady
transfer, and persisted retirement corrections passed. The two remaining failures
exposed narrower fixture and source omissions. The coverage fixture supplied stable
port identities but still described its fan-in connections only with propagation
indices, so semantic comparison conservatively reported an input change; it now
supplies the complete stable sample connection descriptions. The disconnected Tick
output reserved `history + latency` compact-carry frames but classified all of them as
past frames. It now records the output latency as the carry's future extent, allowing
the transition to reconstruct both halves of the output window in the shared monotonic
Sequential coordinate. Front 6 remains open for another collaborator verification run.

**Front 6 completed and verified (2026-10-21):** the collaborator's following full
548-test run passed. The final run verifies the corrected semantic-coverage fixture and
the disconnected Tick-output history/latency reconstruction together with the rest of
the suite. No known Front 6 correctness boundary or unresolved implementation friction
remains. Front 6 is closed and active implementation moves to Front 7. The possible
per-node/per-pace grouping of callback ordinal stores remains measurement-driven
cleanup, not transition correctness work and not a Front 6 completion condition.

**Front 6 follow-up exposed by Front 4 (2026-10-23; resolved by representation and
verified):** a Sequential input supplied through
`TickInvocationCall`, alone or mixed with a realtime contribution, had no
recoverable node-owned history, so a graph revision that changed its delivery kind
could not preserve it. The reopened Front 4 item gives every such input with history
a node-owned retained timeline. Its realization is ordinary `direct` retained state,
so the completed Front 6 transfer and transition-suffix paths apply unchanged and
this front needs no delivery-kind special case. The superseded cross-kind machinery
and its friction record are retained in [Background-fed input playback
history](./historical/background_fed_input_playback_history_2026-10-23.md). The
end-to-end revision fixtures recorded under Front 4 passed, so Front 6 is closed
again.

**Post-completion raw-migration audit correction (2026-10-24; human verification
pending):** the new node-owned sample and event input timelines were semantically
visible to Front 6 through `RealtimePortStateRequirement::stable_identity`, but their
exact-shape `NodeStorage` raw-region migration identities still used
`NodeBundleHandle` and input-port index. Those are generation-local coordinates. A
different stable node which reused the same coordinates and physical timeline shape
could therefore receive the predecessor's bytes before Front 6 correctly found no
semantic state match.

Lowering now derives the raw-region identity from the same stable concrete node,
input-port name/index/kind, direction, and `sequential_input_history` role used by the
semantic inventory. The physical storage kind, value/event shape, pace, retained
extent, and capacity remain suffixes used only for exact-shape migration. An anonymous
input or a zero-history input has no stable semantic state identity, so its persistent
timeline carries an empty raw migration identity and is initialized for the new
generation. Sample and event adversarial fixtures cover both sides: equal numeric
bundle/port coordinates with different stable identities do not migrate, while the
same stable port continues to migrate after every bundle handle is shifted. Front 6 is
reopened only through the collaborator's compile and full-suite verification of this
bounded correction.

The first verification run passed 597 of 598 tests. The only failure was in the new
bundle-handle-shift fixture: `GraphBuilderNodeBundles::import_child()` correctly clears
the imported bundles' inverse `virtual_node_handles()` lists because normal embedding
rebuilds them through `GraphBuilderVirtualNodes::import_child()`. The fixture preserved
the original virtual-node IDs instead but initially rebuilt only the forward
`VirtualNodeRecord::node_bundle_handles` side. It now restores the inverse lists after
shifting the records, so the changed-bundle graph retains the intended stable concrete
node identities. No production migration behavior changed in this follow-up; another
collaborator verification run remains pending.

### Front 6 raw-migration correction verification (2026-10-24)

The collaborator's next full run passed all 598 tests. This run verifies the stable
input-port migration identities and the corrected bundle-handle-shift fixture. No
known Front 6 correctness work or implementation friction remains, so Front 6 is
closed. Front 3 is the next dependency-ready correctness front.

## Front 7: remaining GraphJit and node execution semantics

**Primary ownership:** legal authored node/layout constructs that still have explicit
lowering guards or incomplete runtime scheduling semantics.

This front owns, among other remaining gaps:

- nested declarations;
- declared arrays/auxiliary/compiler-owned regions;
- remaining external-storage layout forms;
- activity/TTL semantics;
- actual scheduling of skip callbacks rather than callback discovery alone;
- detach/event lowering edge cases;
- tiled/subgraph connection-analysis gaps;
- moving background-only `TockState` out of canonical realtime `NodeStorage`;
- normalized block replayability based on semantic requirements rather than whether
  the source author happened to write `tick()`.

Replayability should eventually be decided from the normalized `do_tick_block()` plus
resolved state/history/latency/pace facts. A native `tick_block()` or batch-only node
may therefore be replayable at complete legal invocation quanta. `tick()` remains a
first-class optimized authoring form because its generated batch adapter can expose
cross-node SIMD opportunities.

**Dependencies:** front 1 for final realization predicates; front 2 for normalized
batch operations. Other sub-items can proceed earlier.

**Entry assessment (2026-10-21):** Front 6's verified completion satisfies the
state-continuity prerequisite for this front. The first dependency-ready work should
separate background-only `TockState` from canonical realtime `NodeStorage` before
opening nested declarations and their nested-background-state spans in GraphJit. This
is a receiving-structure correction, not a prerequisite invented by the transition
implementation: the normative storage model already makes `TockState` a
background-owned sidecar, while the current lowering still requires a canonical
`NodeStorage` offset for it. Implementing nested declarations first would extend that
known temporary ownership into more layout forms and create avoidable migration work.
After the sidecar boundary is complete, nested declarations plus declaration-owned
regions form the next coherent authored-layout slice; activity/TTL plus actual skip
scheduling form a separate subsequent scheduling slice.

**Active Front 7 lifecycle friction (2026-10-21; collaborator decision required):**
the intended storage split cannot be implemented faithfully through the current
authored lifecycle ABI. `InitializationContext`, `MoveContext`, and `ReleaseContext`
simultaneously expose realtime `State`, background-only `TockState`, and
`NodeStorage`; each erased lifecycle callback consequently receives one
`NodeStorage` containing both objects. Realtime and background execution currently
work around that coupling by constructing two complete `NodeStorage` instances. The
background copy supplies `TockState`; the realtime copy therefore also allocates an
unused `TockState`, and generation migration can execute authored `TockState` work on
the realtime cutover boundary.

Separating only the allocation is not a valid fix. A node may define both state
types and one authored callback may access both. Giving realtime lifecycle a null or
temporary `TockState` changes that callback's contract; retaining a sidecar beside
realtime storage preserves the wrong ownership and realtime migration work; invoking
the combined callback once per domain duplicates arbitrary authored side effects;
and initializing only the background instance leaves realtime `State` lifecycle
incomplete.

The recommended dependency change is to split authored lifecycle by execution
domain before implementing the sidecar. Keep realtime `initialize`/`move`/`release`
contexts limited to `State`, realtime declaration-owned storage, resources, and
`NodeStorage`. Add a distinct background lifecycle context limited to `TockState`,
configuration, and resources; construct and release it under `BackgroundExecutor`.
Initially rebuild `TockState` for each generation instead of migrating it, because it
is explicitly non-semantic acceleration and compatible retention is only an optional
optimization. Nodes with only one state kind retain one obvious lifecycle; nodes with
both kinds must author the two domain callbacks explicitly rather than relying on one
ambiguous combined callback. After this boundary is selected, remove TockState-owned
declaration regions and `DeclarationContext::tock_state()`, implement the background
sidecar, and then open nested realtime declarations.

**Accepted Front 7 lifecycle resolution and source implementation (2026-10-21;
collaborator verification pending):** the collaborator accepted the domain split
above. Realtime authored lifecycle remains `initialize(InitializationContext<Node>)`,
`move(MoveContext<Node>)`, and `release(ReleaseContext<Node>)`; those contexts now
expose only realtime `State`, realtime declared storage/resources, and `NodeStorage`.
Background authored lifecycle uses overloads of the same operation names with
`TockInitializationContext<Node>` and `TockReleaseContext<Node>`, which expose only
`TockState` and resources. There is deliberately no Tock move callback.

Source now builds a separate `TockStateLayout` beside `NodeLayout`. `NodeStorage`
contains only realtime state and declared regions; `DeclarationContext::tock_state()`,
Tock-owned local/import/export arrays, and nested-background-state spans are removed.
`BackgroundExecutor` owns one compact `TockStateStorage` sidecar allocation per
execution generation, with aligned fixed Tock objects indexed independently from
realtime layout offsets. A pending generation allocates only unconstructed sidecar
memory, while the background worker constructs and initializes it on first use or
successor reconciliation and releases/destroys it on the same worker. Each successor
starts with fresh Tock state; only semantic background coverage/materialization
reconciliation migrates. Generated background roots now receive one call-frame
pointer, and each authored Tock frame is prebound to its indexed sidecar slot, so
generated background code has no realtime `NodeStorage` base from which it could
address `State`.

This resolves the recorded lifecycle friction without retaining a compatibility
sidecar on the realtime path. The next dependency-ready Front 7 slice is nested
realtime declarations plus their declaration-owned regions. Compilation and test
verification remain with the collaborator; a failure there reopens this boundary at
the failing invariant rather than changing the accepted ownership model.

**Accepted Front 7 primitive-domain split and source implementation (2026-10-21;
collaborator verification pending):** a concrete executable primitive now authors
exactly one primary callback family: one of `tick()`/`tick_block()` (and later
`tick_block_batch()`), or one of `tock_coverage()` (and later
`tock_coverage_batch()`). Tick primitives declare only Tick outputs; Tock primitives
declare only Tock outputs. A module node is the explicit grouping mechanism when one
authored abstraction needs work in both domains.

Compiler records now omit Tick/skip anchors for Tock primitives. Package scanning and
lowering accept that exclusive record shape, imports are conditional on the selected
family, and connection scheduling gives only Tick primitives realtime execution
positions. Tock-only primitives remain present in semantic/background planning but
cannot appear as implicit no-op Tick steps. Replay preserves the deliberate
asymmetry: an eligible Tick wrapper may run in a generated background replay program,
but authored Tock never creates an implicit Tick callback. The superseded dual-family
policy is preserved in
[the historical callback-policy record](./historical/dual_tick_tock_primitive_callback_policy.md).
After collaborator compilation/test verification, nested realtime declarations and
their declaration-owned regions remain the next dependency-ready Front 7 slice.

**Nested declaration/layout slice implemented in source (2026-10-21; collaborator
verification pending):** GraphJit no longer assumes that graph primitive index and
`NodeLayout` node index are identical. Lowering retains a map from each top-level
primitive to its layout node. Nested declarations can therefore occupy layout records
without entering graph execution, configuration, port, or background namespaces.
The map supplies each primitive `State` offset and authored-Tock sidecar index.

The same slice admits authored realtime local arrays, nested-node-state spans, and
typed import/export array bindings. The existing `NodeLayout` remains their only
implementation. It owns initialization order, pointer patching, lifecycle, and
migration. A runtime fixture puts two nested layout nodes before later top-level
primitives. The fixture combines local, nested, exported, and imported storage. It
makes sure that lifecycle and Tick calls address the correct state and Tock sidecar
records. GraphJit still rejects arbitrary raw regions that have no compiler owner.

After collaborator compilation/test verification, this closes the nested-declaration
and declared-array portion of Front 7. Activity/TTL and actual skip scheduling are the
next dependency-ready coherent slice; the other guarded edge cases remain separately
tracked above.

**Front 7 activity/TTL friction recorded before resolution (2026-10-21):** the stored
TTL fields did not define activity for generalized continuous
values. The removed graph runtime used two `Sample`-only tests. It compared input
samples for a constant unchanged value, and it compared output samples with zero.
The current continuous-value registry has no type-owned equality or clear-value test.
A byte comparison is not correct for floating-point signed zero or storage padding.
A `Sample`-only implementation will also break the generalized Front 1 path.

GraphJit also has no project default for TTL. A node instance can supply an explicit
TTL, and a node type can supply a default TTL. If both are absent, the old runtime used
the project default. `GraphJitConfig` currently supplies no such value. Treating the
missing value as infinite preserves execution, but it silently removes the former
project policy.

The recommended resolution adds type-owned block operations to the sole
continuous-value registry. The operations test equality with one remembered value and
test equality with the type's cleared output value. GraphJit can call each block
operation once per relevant frontier port, then propagate compact activity facts
through the generated schedule. It stores only a dormancy flag, a TTL counter, valid
bits, and one remembered value for each dynamic input. A stable compiler-owned raw
region retains that state across compatible generations. Static constant inputs need
no scan. Event activity uses empty-sequence facts. `GraphJitConfig` must also receive
the project default TTL, with infinity as its explicit default value. This design keeps
type switches in the registry and avoids repeated scans at downstream fanout points.

**Accepted Front 7 activity/TTL resolution and source implementation (2026-10-21;
collaborator verification pending):** TTL is opt-in. An explicit instance TTL wins.
The node-type TTL is the fallback. If neither value exists, TTL is disabled. The
compiler does not add activity storage, scans, or branches in that case. An explicit
infinite TTL also disables the feature. `GraphJitConfig` does not contain a project
default. The activity-summary optimization pass now exists. A later project or user
setting will provide a configurable default. Its exact owner remains open. This
supersedes the earlier proposal to add a project default now.

The continuous-value registry now owns semantic value equality and cleared-value
tests. This keeps signed-zero and dynamic-value rules out of generic GraphJit code.
Each enabled primitive owns a compact compiler raw region. It contains the dormancy
flag and the elapsed quiet-sample count. Dynamic continuous inputs also need validity
bits and remembered values. Equal ordinary fanout inputs share that comparison state
when both consumers use the same unsliced root window. Exact compatible generations
can migrate these regions. Static constant inputs need no state or scan. Empty event
windows contribute activity facts without persistent remembered values.

Generated execution checks input activity before a callback. A dormant primitive uses
its skip callback while its inputs remain unchanged. A changed input wakes it. After a
Tick callback, constant inputs and cleared continuous or empty event outputs advance
the TTL. A changed constant input starts a new quiet interval, so quiet time from
before a wake cannot make the primitive dormant again too early. The TTL counter uses
project samples and saturates on overflow. Primitive slicing applies the same rule to
each legal callback slice.

Generated lowering now reuses compact continuous and event activity summaries across
equal fanout branches. A representation epoch invalidates a summary after any planned
write. The summary key also includes the callback window. Continuous comparison state
is shared only for unsliced acyclic consumers of the same root window. SCC callbacks,
privately sliced callbacks, and external Tick-frame overlays keep independent state.
This optimization does not change the accepted TTL policy or runtime ABI. Focused
runtime coverage verifies shared ordinary fanout, opt-in dormancy, wake-up on a
changed constant input, and the disabled default. Compilation and test execution
remain with the collaborator.

**Resolved Front 7 structural-routing friction (2026-10-21):** the planned assumption
was that connection analysis could replace each
tiled or subgraph port with concrete endpoint ports. The existing connection plan
could then consume the result without a new intermediate representation.

The configured graph does not contain only endpoint aliases. A tiled sample input is
one semantic channel layout that spans several concrete input ports. Its conversion
must occur before the result is projected to those members. Splitting the connection
into unrelated mono edges can change mono-to-stereo duplication, stereo projection,
and other conversion semantics. Tiled event inputs broadcast one stream, while tiled
event outputs merge member streams. These operations are also route stages.

A subgraph boundary creates a chain of configured connections. Each segment in that
chain can carry detach metadata. At the time of this friction report,
`SampleConnectionPlan` and `EventConnectionPlan` contained only one optional detach
for a complete connection. Collapsing the chain could therefore lose a delay or a
sample initial value. Keeping the boundary as an execution node was also incorrect
because it has no callback, state, or storage identity.

Adding cases directly to connection inventory would make scheduling, SCC construction,
sample/event storage, background dependencies, transition identities, and replay
planning understand subgraph, boundary, and tiled `NodeBundle` ports. It would also
require each phase to reconstruct the same conversion and detach ordering.

The initial recommendation was a structural-routing normalization pass before ordinary
connection analysis. Its output would have contained concrete endpoints and an ordered
route of conversion, projection, broadcast, merge, and detach stages. This would have
made one more representation for information that `ConfiguredGraph` already retains.

The accepted correction keeps the authored structure in `GraphBuilder` and
`ConfiguredGraph`. `build_structural_sample_routes()` and
`build_structural_event_routes()` traverse the configured connections while GraphJit
builds the ordinary connection plans. They compose sample channel mappings and event
conversions across subgraph, boundary, and tiled `NodeBundle` ports. They expand tiled
sample targets after semantic channel conversion, broadcast tiled event inputs, and
merge tiled event outputs. They do not create a persistent general route graph or
runtime routing objects.

Each concrete source dependency can contain at most one detach. GraphBuilder rejects
a second `detach()` call when any source in the expression is already detached.
Structural traversal also rejects two detach records that become serial operations on
the same concrete source dependency across a subgraph boundary.

This restriction follows from the graph invariant. GraphJit removes detached
dependencies to form the execution DAG. Each removed dependency must close a cycle
when restored while all other detached dependencies remain removed. Two serial
detaches on one route cannot both satisfy that requirement. The existing
`validate_detached_connections()` path already enforces detach necessity for supported
concrete graphs. The structural traversal must preserve this validation after bundle
expansion.

Front 7 work now implements the structural traversal and its tests first. It then
removes the tiled/subgraph inventory guards and feeds concrete routes through existing
sample, event, background, SCC, and transition planning. Native block and batch replay
remains after Front 2 because its
legal invocation quanta come from normalized callback operations. Non-pointwise replay
with Random Access inputs also needs an explicit Tick-replay coverage propagation
contract. The current propagation callbacks belong only to authored Tock primitives.

**Front 7 non-detached structural routing implemented in source (2026-10-21;
collaborator verification pending):** connection analysis resolves configured
connections whose endpoints are tiled, subgraph, or internal boundary `NodeBundle`
ports. Only concrete primitive bundles enter execution inventory. Lowering keeps the
same concrete primitive list.

The traversal resolves subgraph input and output aliases in both directions. It
projects a tiled sample input to its mono members after semantic conversion. It merges
tiled sample outputs before the next conversion. It broadcasts tiled event inputs and
merges tiled event outputs. The final connection plans contain only concrete endpoints
or the root boundary.

Serial sample conversions are multiplied into one target-major coefficient transform.
Realtime composition and background materialization consume that transform directly.
A subgraph boundary `NodeBundle` therefore does not add a runtime conversion layer.
Serial event conversion steps are concatenated into one checked conversion program.
Focused tests cover a subgraph sample input with two conversions, tiled sample input
projection, and a two-step event conversion across a subgraph input.

**Front 7 per-source detach representation implemented in source (2026-10-22;
collaborator verification pending):** `ConfiguredSampleConnectionSource` and
`ConfiguredEventConnectionSource` pair each configured sample channel or event source
port with its optional detach settings. Structural routing later expands a tiled or
subgraph `NodeBundle` source to concrete primitive ports. `SamplePortExpression` and
`EventPortExpression` preserve the same information while GraphBuilder constructs an
expression. The binary configured-graph archive stores the detach beside each source.

`EventPortRef(GraphBuilder&, std::span<EventPortRef const>)` forms event fan-in from
existing event expressions. It concatenates each expression's sources and detach
settings. The older constructor that accepts `EventOutputPortId` values still creates
an undetached expression. Code does not need to extract `EventPortRef::sources()` and
lose detach settings when it combines event expressions.

Structural sample routing carries detach settings through channel transforms and
target projection. Structural event routing concatenates its existing non-expanding,
timestamp-preserving conversion program and carries the detach on each resolved event
source. It does not introduce separate conversions before and after detach storage.
This follows the dependency meaning of `detach()` and removes the proposed conversion
split.

GraphJit validates each detached event source as a necessary cycle-breaking
dependency and binds it to the consumer SCC. Uniformly detached event sources with
equal settings retain the existing shared feedback buffer. A connection that mixes
ordinary and detached sources, or different detach latencies, keeps the producer
group's source-index sidecar for its ordinary branches. Each detached
`EventSourcePlan` reads the exact callback-local or source-converted representation
that also feeds the producer-group merge. Immediately before each consumer, GraphJit
selects ordinary branches from the retained fan-in representation, combines them with
the delayed source streams in authored source order, and applies the one fused event
conversion. No authored detach identity or general route-stage graph exists. The
superseded merged-sequence filtering implementation is preserved in
[`historical/front_7_merged_event_detach_filter.md`](historical/front_7_merged_event_detach_filter.md).

Focused tests cover all four filtered feedback runtime combinations: sequence or ring
source into sequence or ring feedback storage. A GraphJit runtime test combines one
detached `EventFeedbackA` source and one ordinary `TriggerEventSource` source before
`EventFeedbackB`. This case also covers an acyclic producer feeding a sliced SCC.
Connection-plan tests cover mixed ordinary and detached sources and two detached
sources with different latencies. Compilation and test execution remain with the
collaborator.

The rejected conversion-split proposal is preserved in
[`historical/front_7_single_route_detach_split_false_start.md`](historical/front_7_single_route_detach_split_false_start.md).

**Front 7 event-route representation friction recorded before resolution
(2026-10-22):**
`ResolvedEventBranch` can carry a different `source_type` and
`EventConversionPlan` for each source that reaches one authored event fan-in.
`RoutedEventConnection` and `EventConnectionPlan` still require one shared source
type and one shared conversion. `build_structural_event_routes()` therefore creates
separate `RoutedEventConnection` records when the resolved conversions differ. If
those records have the same concrete `EventInputPortId`, event lowering tries to bind
that input twice and rejects the second binding.

A concrete valid case is one subgraph output declared as `EventTypeId::trigger` and
backed by a MIDI-to-boundary-to-trigger chain, plus a second trigger output backed by
a boundary-to-trigger chain. Combining those two `EventPortRef` values creates one
trigger fan-in. The resolved terminal sources and conversion programs differ even
though the authored fan-in type is the same.

The required representation keeps a conversion from each resolved source to the
authored fan-in source type. Lowering must convert each source to that type, merge the
converted streams in authored source order, and then apply the one common connection
conversion. The same change must make canonical raw event output storage reusable by
`EventOutputPortId`. The current exact-source-set `EventProducerGroupPlan` can assign
one output to two groups when that output participates in two different fan-in sets,
while `PrimitiveEventOutputBindingPlan` permits only one storage binding. Solving only
the differing-conversion example would leave this related ownership error in place.
Do not add a special rejection or another connection-wide conversion split. This is
the next event-planning receiving-structure change.

**Front 7 event-route representation resolved in source (2026-10-22; collaborator
verification pending):** `EventSourcePlan` now keeps the concrete raw `source_type` and
the `EventConversionPlan` from that type to `EventConnectionPlan::source_type`.
`build_structural_event_routes()` keeps one `RoutedEventConnection` for the authored
fan-in. It composes each structural branch only as far as the authored source type.
Realtime and background materialization convert each raw source first, merge the
converted streams in authored source order, and apply `EventConnectionPlan::conversion`
once after the merge.

`PrimitiveEventOutputBindingPlan` now owns one callback-facing raw sequence and, when
history or latency requires it, one canonical raw sequence for each concrete
`EventOutputPortId`. Several `EventProducerGroupPlan` records can read the same raw
sequence, so one output can participate in different fan-in sets without acquiring a
second callback binding. A retained acyclic identity connection still aliases the raw
canonical sequence and does not add a derived copy.

A multi-source `EventProducerGroupPlan` always uses a separate fan-in representation.
It does not use one source's raw `EventOutputPortId` representation as the aggregate.
This keeps the raw output valid for another fan-in set and for a later graph
transition. The superseded producer-home design is preserved in
[`historical/front_7_event_fan_in_producer_home.md`](historical/front_7_event_fan_in_producer_home.md).

The direct alias exposed one transition case that the earlier indexed fan-in storage
had hidden. If an event output and input share one steady sequence, their predecessor
states cannot both be transferred into it. Transition specialization now starts that
shared steady sequence fresh, gives each semantic state its own transition-only ring,
merges new callback-local events into the output ring after the producer, and continues
to materialize current delivery into the input ring before the consumer. The prior
assumption is preserved in
[`historical/front_6_event_output_transition_suffix_assumption.md`](historical/front_6_event_output_transition_suffix_assumption.md).

Focused connection and runtime tests cover different source conversions in one fan-in,
one output used by two fan-in sets, conversion before merge, and transition of shared
retained event output/input storage. Compilation and test execution remain with the
collaborator.

### Front 7 detach semantics correction (2026-10-21)

`detach()` marks dependencies that GraphJit removes to break a cycle. It does not
create storage or select a storage layout. GraphJit makes those decisions later.
Therefore, GraphBuilder does not need an authored detach identity.

Equal detach settings on the same source contributions have equal meaning. For
example, `g.tile<stereo>(l, r).detach(1)` and
`g.tile<stereo>(l.detach(1), r.detach(1))` must produce the same configured
representation. Storage planning can combine or separate the retained values without
changing that representation.

Calling `detach()` when any source contribution is already detached is an error. This
rule also applies when the existing and requested settings are equal. The equivalent
forms above remain valid because neither form applies `detach()` twice to one source
contribution.

`SamplePortExpression` and `EventPortExpression` now keep optional detach settings for
each source contribution. GraphJit preserves those settings through sample conversion,
target projection, event conversion, SCC validation, and storage planning. This does
not create a detach identity.

The previous identity proposal is preserved in
[`historical/front_7_detach_identity_false_start.md`](historical/front_7_detach_identity_false_start.md).

### Front 7 dependency-ready status (2026-10-22)

All Front 7 work that does not depend on Front 2 is implemented in source. This
includes the `TockStateLayout` ownership split, the exclusive Tick-or-Tock primitive
callback rule, nested declarations, local/shared array bindings, opt-in activity and
skip scheduling, tiled/subgraph routing, per-source detach settings, and the event-route
representation described above.

`plan_declarations()` still rejects a `NodeLayout::Region::Kind::raw` region that has
no GraphJit owner. This is a deliberate validation. `NodeLayoutBuilder::declare_raw_region()`
does not create a node-state field binding, so accepting such a region would allocate
bytes that an authored callback cannot address. GraphJit-owned sample, event, activity,
and transition raw regions remain supported through their exact plan records.

Root sample/event boundary storage also remains outside this front. Concrete system or
communication nodes provide those boundaries, as specified for Front 8. Normalized
block replayability remains deferred until Front 2 provides normalized batch operations.
Front 7 is therefore source-complete at its current dependency boundary. Collaborator
compilation and test verification are still pending.

**Front 7 verification follow-up (2026-10-22; collaborator re-run pending):** the
first full run after the event producer-home and per-source detach changes passed 555
of 572 tests. Four projected-sample tests still inspected the superseded
`SampleConnectionPlan::detach` and `projection_contributions` fields; they now inspect
`routed_contributions`, the two mono feedback timelines, and the final target-port
composition. Two alignment-migration tests used the superseded
`graphjit.sample.composed_feedback_alignment` identity prefix; routed feedback now
owns the same alignment ring under `graphjit.sample.routed_feedback`.

The runtime failures exposed one transient-arena defect. An `EventMergePlan` with
`preserve_existing_target == true` can extend its target after every primitive
invocation in a cyclic `ExecutionRegionPlan`. Its transient lifetime began at the
merge primitive, so an earlier primitive on the next SCC slice could reuse and
overwrite the accumulated sequence. Event lifetime planning now keeps that target
live across the complete cyclic region. This adds no representation or realtime
operation, but the packed transient arena can grow when it previously reused that
memory illegally.

The same run exposed one Tick/Tock-domain omission: disconnected sample-port planning
requested a realtime execution position for a Tock-only primitive. Tock sample and
event ports come from `BackgroundNodeCall` batch bindings, so `plan_sample_ports()`
and `plan_event_ports_once()` leave their realtime primitive binding records empty;
disconnected Tick sample/event planning, event transient-lifetime planning, and
execution-operation planning also skip those primitives. The remaining
failures were stale assertions: `midi_to_trigger` emits two note-on triggers rather
than the source's note-off, raw event producer ownership adds one expected region,
the final packed-stack diagnostic supersedes a local chooser diagnostic, and already
separate event producer/input state needs no transition-only root for an unchanged
graph. These corrections require collaborator compilation and full-suite verification
before Front 7 is closed.

**Second Front 7 verification follow-up (2026-10-22; collaborator re-run pending):**
the next run passed 566 of 572 tests. The two sample-feedback migration tests still
required the old `:samples` suffix even though routed alignment-ring identities now
end at their `:alignment=` extent. Their lookup now accepts the routed identity.

The mixed ordinary/detached event test exposed a source-cursor error. The first
implementation filtered a detached source out of the sorted producer-group sequence.
A later SCC slice can insert a new event before the saved numeric cursor, so that event
was never appended to the delayed stream. `EventPortBindingPlan` now records the exact
per-source representation used by each producer-group merge. A detached
`EventFeedbackPlan` reads that representation and, for its callback-local source,
starts at element zero on each append scope.

The retained-event transition test was not stale. The single-source producer-group
carry contains both the input-history interval and the source-output future interval,
but its `EventRealtimePortStateRealization` described only the input requirement.
Input realizations now record the exact output-latency requirements and conversions
whose future events are present in their retained producer-group storage. Transition
planning uses one supplemental `EventRealtimePortStateTransfer` to seed that future
interval at cutover. Steady execution still merges only the callback-local source, so
ordinary calls do not duplicate already-retained future events. The compact-retention
inspection also now matches each
`RealtimePortStateRealization::retained_storage` to its own `NodeLayout` region; the
output and input roles intentionally use different compact carries.

Finally, a callback cannot write directly into a full persistent event ring because
`EventOutputPort` requires producer overflow telemetry. Raw-output storage costing now
includes the required callback-local sequence for the full-`NodeStorage` candidate.
This lets the final packed-stack check report the requested unavoidable-budget
diagnostic instead of failing earlier on a missing telemetry field. These source and
test corrections await collaborator compilation and the next full-suite run.

**Front 7 full-suite verification and adversarial completion audit (2026-10-22):**
the collaborator's next run compiled and passed all 572 tests. This verifies the
source corrections in both follow-ups above and closes their recorded blockers.

The completion audit found four documented invariants that the passing suite did not
exercise directly. New focused tests now require structural sample and event routing
to reject two serial detach records that become adjacent only after subgraph-boundary
expansion. They also require `validate_detached_connections()` to reject one
unnecessary detached source even when another source in the same sample or event
connection closes a real cycle. A generated-runtime test now carries an enabled
primitive's compiler-owned activity region across an exact compatible generation and
requires the successor to skip at the inherited TTL boundary rather than starting a
fresh quiet interval. Another generated-runtime test requires an empty event input
window to use `skip_block()` after TTL expiry and a later non-empty window to wake the
primitive.

These tests add no implementation or semantic change. Collaborator verification of
the new tests remains pending. Front 7 remains source-complete at its current
dependency boundary. Normalized block replayability still waits for Front 2's
normalized batch operations and is not an unimplemented part of this dependency-ready
slice.

The first adversarial full-suite run passed 575 of 576 tests. The sole failure was in
the new `CompilerOwnedActivityStateMigratesAcrossCompatibleGenerations` test: its
expected authored `ActivityProbe::State::tick_calls` value incorrectly assumed that
the authored state migrated. This fixture initializes authored node state afresh. The
compiler-owned activity region migrates separately, as shown by the successor's
immediate `skip_block()` call. The assertion now expects zero successor tick calls and
one successor skip call. A collaborator rerun remains pending.

**Front 7 adversarial completion verification (2026-10-23):** the collaborator's
following full run passed all 576 tests. Front 7 is complete for every contract that
does not depend on Front 2. Its remaining normalized block-replay integration is part
of the Front 2 callback-normalization pass rather than an independently implementable
Front 7 slice.

**Front 7 dependency completion (2026-10-23):** Front 2 now supplies normalized
scalar and batched Tick operations, including one-lane replay of a batch-only Tick
callback. The collaborator's 592-test run verifies that replay path with the completed
Front 4 delivery path. The final Front 2 dependency is therefore satisfied and Front 7
is complete.

**Remaining-front dependency re-audit (2026-10-23):** with Fronts 1, 6, and 7 closed
at their stated boundaries, 13 numbered fronts remain (`2`-`5` and `8`-`16`), not 14.
The older summary below remains useful as an ownership graph, but later implementation
work has overtaken two of its status edges. The Front 4 Tick-time dynamic-binding
checkpoint is marked complete in
[`coverage_and_background_evaluation.md`](./coverage_and_background_evaluation.md),
and current source contains `TickInvocationWorkspace`,
`TickMaterializationSnapshot`, persisted sample/event playback, and missing-position
neutrality. Front 4 therefore needs a focused closure audit and status cleanup, not a
new prerequisite implementation pass. Front 5's producer reserves, pending queues,
capture disposition, transaction consumption, and persisted-state mailbox have also
landed. Front 5 remains open for generalized continuous-value capture/publication and
the separately recorded bulk-span performance work.

**Front 5 follow-up (2026-11-01):** the generalized continuous-value
capture/publication gap described above is complete and verified. The
repository-owner build succeeded, and the full suite passed all 661 tests. The
separately recorded bulk-span work remains Front 16 rather than an open Front 5
correctness dependency.

Fronts 2, 3, 4, 5, 6, 7, and 8 have now completed the correctness execution path. The
remaining execution-path order is:

1. The Front 16 measurement/baseline step required before structural optimization.
2. Front 12 scoped builder migration.
3. The remaining Front 16 optimizations after the correctness architecture and
   builder path are measurable together.

Front 2 reached source implementation on 2026-10-23. Full-suite verification remains
pending after its Front 4 background-delivery dependency. Any Front 2 verification
failure reopens the exact callback, ABI, scheduling, or lowering boundary that fails.

The re-audit's claim that Front 4 needs only a closure audit no longer holds.
Background-only Sequential sample and event inputs have a partial implementation, but
mixed Tick/background delivery and disconnected Random Access inputs remain rejected.
Front 4 therefore still needs an implementation pass.

**Post-verification update (2026-10-23):** the implementation pass described above is
complete. Fronts 2 and 4 passed the complete 592-test suite, and Front 7's normalized
block-replay dependency is satisfied. Eleven numbered fronts remain outside those
closed fronts, plus the bounded reopened Front 6 mixed-input transition case described
above. Complete that transition case before starting an unrelated front. Front 3 is
then the next dependency-ready correctness front in the execution-path order.

**Reopening update (2026-10-23):** the Front 6 case was traced to Front 4's playback
representation, which rebuilt background-fed history from the snapshot instead of
keeping the node-owned history. Front 4 was reopened for node-owned input timelines.
That work and its end-to-end fixtures passed the collaborator's full suite, closing
both Front 4 and the Front 6 case. Front 3 is the next dependency-ready correctness
front.

The control-plane order remains Front 9 before complete Front 10 and Front 13, then
Front 14 and Front 15 after their stated inputs exist. Front 10 may continue to use
opaque configuration source while Front 9 lands, but cannot complete typed replay
without Front 9. Front 11 remains independent and may be scheduled wherever package
build/cache cost becomes the highest practical impediment. This is a dependency order,
not a requirement to delay independent control-plane or Front 11 work.

## Front 8: application execution and system-audio integration

**Primary ownership:** make the new execution architecture the application's normal
runtime path.

**Status:** complete and verified by the repository owner's 667-test full-suite run on
2026-11-02.

This front owns:

- construction and lifetime of the `GraphExecutor` app module plus its internal
  `RealtimeGeneration`, `BackgroundExecutor`, and `AsyncCapacityManager`;
- one `ProjectGraph -> GraphExecutor` staging bridge after successful `GraphJit`;
- complete off-thread construction of one `ExecutionGeneration` containing both actor
  realizations, routes/endpoints, pre-sized work descriptors, migration data and all
  cutover resources before its pointer can become pending;
- realtime-authoritative pass-boundary cutover: final old-generation chain publication,
  allocation-free ordered cutover publication to background, then realtime swap;
- background completion of already-selected old-generation work, closed old-generation
  queue drain, and prepared migration before consuming new-generation queues;
- preservation of multiple ordered cutovers while background lags;
- realtime compiled-generation state migration and pass-boundary activation;
- audio callback routing through `GraphExecutor::activate_pending()` and
  `GraphExecutor::tick_block()`;
- `BackgroundExecutor` worker/evaluation lifecycle and independently pinned
  producer queues;
- `AsyncCapacityManager` provisioning/reclamation for producer-specific
  power-of-two blocks, with separate producer reserves/background pending queues and
  manager-owned allocation granularity;
- one background-to-capacity-manager released-block stream where practical;
- realtime-to-background produced-chain publication and a background-to-realtime
  latest-compatible-persisted-version mailbox;
- retired generation/snapshot/queue-block reclamation scheduling;
- executor/capacity-manager shutdown and application error propagation;
- stable logical system-audio device bindings;
- ordinary project graph input/output nodes using those bindings;
- silence/discard behavior when a persisted logical device cannot currently resolve;
- device-clock buffering/resampling boundaries without mutating project topology.

Automatic graph-node creation for every detected hardware device remains optional
convenience behavior, not a prerequisite for this front.

**Dependencies:** front 4 is the near-term execution prerequisite; enough of fronts 6
and 7 must be present before this becomes the sole production path.

**Front 8 application-entry decision (2026-11-01):** `ProjectGraph` keeps both child
edges. It invokes `GraphJit` once and then invokes `GraphExecutor` once. It plans each
compile from the last executable generation that `GraphExecutor` accepted. The
executor validates the predecessor, constructs the complete `ExecutionGeneration`,
and appends it to its ordered accepted chain. The false start that proposed two
`GraphExecutor` entries or moving `GraphJit` under `GraphExecutor` is preserved in
[`historical/front_8_replaceable_pending_transition_false_start_2026-11-01.md`](historical/front_8_replaceable_pending_transition_false_start_2026-11-01.md).

## Front 9: restricted configuration-expression language

**Primary ownership:** parse persisted/project-authored configuration source into owned
typed argument values without accepting arbitrary C++ execution.

The source is a type-directed argument list. The registered configuration signature
supplies the expected types. The language supports scalar values, arithmetic, pure
allowlisted functions, aggregates, registered containers, enum values, and semantic
references to existing project nodes. It does not support statements, declarations,
mutation, loops, arbitrary calls, or general C++ execution.

Enum values are bare identifiers. The expected enum type controls lookup. The Clang
package-introspection path must emit enum and aggregate metadata for package-defined
configuration types. The parser must not parse arbitrary C++ headers.

A stable project-node reference starts with a durable node-ID literal such as
`&550e8400-e29b-41d4-a716-446655440000`. Three postfix selectors have distinct roles:

- `.name` maps to `ProjectVirtualNodeSelector` and searches only the direct virtual
  nodes in the current subject scope.
- `[n]` maps to `ProjectVirtualMemberSelector` and selects concrete virtual member
  `n`.
- `#name` maps to `ProjectTiledChildSelector` and selects a named tiled child.

`ProjectVirtualNodeSelector` must not search descendant subgraphs. A selected virtual
node supplies the semantic route into its backing subgraph. `ProjectSubgraphSelector`
is not part of the intended semantic path. Configuration references and
`ProjectNodePortMatcher` must use one shared resolver with these rules.

The evaluator must use the registered configuration signature/type information to
produce the same owned typed argument tuples already consumed by `NodeInstances`.
Provider-generated operations may still be used for clone/destroy/equality/hash and
for safe construction of provider-defined value types; that does not require compiling
arbitrary user-authored C++.

The expression/value representation must also support durable node replacement.
An `on_message` handler supplies the successor definition and its complete typed
argument list. A leaf-node handler reads values that it preserves through `this`.
A class-module handler reads values that it preserves through the retained class
instance. Package code converts
the supplied successor arguments to one ordered JSON argument array before the
handler returns. The generated `ConfigurationExpressionTypeSchema` reads scalar,
aggregate-field, and sequence-element values. Node authors do not write a second
serializer for ordinary Front 9 values. `NodeInstances` later reconstructs the typed
arguments from that array and the successor definition's `RegisteredSignature`.

Lists and other structured argument values remain ordinary first-class values. A
handler can pass them as successor arguments when a registered definition uses them.

This front should expose a stable parsed/value representation that fronts 10 and 13 can
use for persistence and semantic replacement without duplicating parsers.

Project-node references separate parsed identity, project-state resolution, typed
materialization, and configured-node ownership. A cache keyed only by source and
definition version cannot reuse project-dependent resolution. If live reference
materialization crosses `BuilderSession` boundaries, that part depends on Front 12 and
must not use an unsafe retained graph handle.

**Implementation status (2026-11-02):** `ConfigurationExpression` parses complete
argument lists and retains their field/index structure. `ConfigurationExpressionTypeSchema`
is emitted from provider C++ by the Clang metadata plugin and package finalizer.
`evaluate_configuration_expression_list()` constructs scalar, enum, aggregate, and
`std::vector`/`std::array` values through provider callbacks. Fixed-sequence element
assignment also remains in provider code; the runtime does not infer an erased
`std::array` layout. `NodeInstances` passes those values through its existing typed
configured-node cache. Configuration
source now survives create/update RPCs, project replay, project save, instance lists,
and instance-change notifications.

`resolve_project_node_path()` now owns `.name`, `[n]`, and `#name` resolution for both
configuration references and `GraphConnections`. Direct-scope virtual lookup is
set-valued. `TiledNodeBundle::channel_type` supplies named tiled children and is
preserved by configured-graph archive version 11. `ProjectSubgraphSelector` is removed
from the project path representation.

The remaining `ConfigurationExpressionTypeKind::node_reference` materialization is an
explicit Front 12 dependency. The evaluator retains and validates the stable reference,
but rejects conversion to `NodeRef` before provider invocation while
`NodeInstances::configure_cached()` uses an independent `BuilderSession`. Front 12 must
provide same-session construction and add project revision/semantic dependencies to
cache reuse. No unsafe cross-session handle or source-only reference cache is present.

**Can start independently:** yes.

Detailed direction:
[Configuration Expression Language Direction](./configuration_expression_language_direction.md),
[Node Definitions And Instances Direction](./node_definitions_and_instances_direction.md),
[Project Graph Application Architecture](./project_graph_application_architecture.md),
and [Project File Serialization Direction](./project_file_serialization_direction.md).

## Front 10: generalized project persistence and graph mutation APIs

**Primary ownership:** the durable desired project graph and transport-neutral mutation
surface.

**Implementation status (2026-11-02): complete and verified.** The collaborator's
full suite passed all 697 tests. `ProjectPersistence` now
normalizes all project node and connection commands before one
`ProjectReplaceGraphRequest`. Structured matcher paths, port/channel selectors,
sample channel types, and event types round-trip through project files and the
JSON-RPC adapters. `NodeInstances` and `GraphConnections` contribute their
durable desired records directly to save collection. The mutation wire surface
uses the definition-kind-agnostic `nodeInstances.*` names.

This front owns:

- stable project node IDs;
- definition ID plus restricted configuration-expression source;
- generalized node create/update/delete;
- complete `GraphConnections` persistence;
- structured `ProjectNodePortMatcher` persistence;
- connection create/update/delete;
- dangling matcher preservation;
- channel selector/type information;
- replay as one normalized project transaction;
- persistence collection from `NodeInstances` and `GraphConnections`;
- typed application requests/events for the same graph mutations;
- JSON-RPC adapters for those typed requests;
- richer/adversarial matcher-path coverage.

This front persists the expression source/AST/value request owned by front 9; it does
not own the expression parser or `NodeInstances` cache.

**Can start in parallel with front 9:** yes, using opaque source strings until the
restricted parser lands.

## Front 11: package artifact provenance and caching

**Primary ownership:** artifact ownership across built-in, common, and project-local
packages.

**Implementation status (2026-11-03): complete and verified.** The collaborator's
full build and test suite passed. `PackageProvenance` now travels from
`PackageDiscoveryRoot` through `IvPackageDeclaration`, `PackageBuildRequest`,
`PackageRevision`, `PackageDefinitions`, JSON-RPC, and the VS Code module view.
Overlapping roots emit one declaration with explicit ownership. CMake prebuilds the
staged built-in package into `IV_BUILTIN_PACKAGE_ARTIFACT_ROOT`; common packages use
the shared global cache; project-local packages use the containing project's
`build/iv`. Artifact signatures include package content and the relevant ABI,
toolchain, target, host-library, PCH, plugin, finalizer, JUCE, and package-build
template inputs.
See [Package Pipeline Application Architecture](./package_pipeline_architecture.md)
for the exact records, configuration keys, and paths.

This front owns:

- explicit package provenance;
- overlapping-root deduplication;
- executable-owned built-in compiled artifacts;
- build/install-time prebuilding;
- installed executable artifact/cache locations;
- correct invalidation keys;
- shared common-package cache ownership;
- project-local build/cache ownership;
- avoiding per-project recompilation of unchanged built-ins/common packages.

It should preserve the existing `PackageWatcher -> PackageJit -> PackageDefinitions ->
NodeDefinitions` responsibility split.

**Can start independently:** yes.

## Front 12: scoped `BuilderSession` / `SubgraphClosure`

**Primary ownership:** same-session scoped graph construction after the current
build/configuration path has been measured and optimized.

This front owns:

- one mutable `BuilderSession` with scoped builders/views;
- zero-copy same-session nesting;
- `SubgraphClosure`;
- stable scoped identities;
- prevention of stale/free builder captures;
- live-reference ownership migration;
- preserving direct frozen `ConfiguredGraph` import;
- equivalent cache-hit/cache-miss semantics.

This front remains deliberately gated until GraphJit plus the two executor modules have become the
normal path and the current builder/configuration path has been profiled.

**Measurement-baseline status (2026-11-05):** the Front 16 builder/configuration
baseline step that gates this front has a host-side benchmark,
`benchmark/iv_graph_configuration_benchmark.cpp` (target
`iv_graph_configuration_benchmark`). Its package definitions are host function
pointers, so it measures only `GraphBuilder`, `BuilderSession`,
`NodeInstances::configure_cached` lookup/miss work, `GraphBuilder::embed`, and root
`finish()`. Scenarios: direct builder operations, cold/warm nested registered-module
trees, a root miss over nested hits, uncacheable arguments at every depth, repeated
`GraphBuilder::subgraph` scopes, cold/warm stereo-tiled modules, and many warm
top-level instances. Each nested tree level is a distinct definition
(`iv.benchmark.tree.N` configures `iv.benchmark.tree.N-1`) because the
`ConfigurationStackEntry` and `begin_builder_module` cycle checks are keyed by
definition ID alone.

Baseline recorded by the repository owner on 2026-11-05 at the pre-Front-12 source
(release build, default shape: depth 3, fanout 3, 8 leaf pairs per module, 32
subgraphs, 64 instances, 1000 direct node pairs, 200 padding definitions, 30
repetitions after 3 warmups). Medians, microseconds:

| Scenario | Bundles | Module calls | Configure | Notes |
| --- | ---: | ---: | ---: | --- |
| `builder_direct` | 2002 | 0 | 2749 | `finish()` 50 |
| `tree_cold` | 2318 | 10 | 2563 | 10 misses, 30 same-batch hits |
| `tree_warm` | 2318 | 0 | 317 | one hit, one `embed` of the whole tree |
| `tree_parent_miss` | 2318 | 1 | 1713 | root miss over nested hits |
| `tree_uncached` | 2318 | 40 | 4271 | every module call misses |
| `subgraphs_cold` | 1608 | 1 | 1344 | |
| `tiled_cold` | 4634 | 11 | 4144 | |
| `tiled_warm` | 4634 | 0 | 655 | |
| `instances_warm` | 14657 | 0 | 3149 | 64 hits, 64 embeds |

Observations from this baseline (implementation status, source-derived):

- *Repeated subtree copying on nested misses.* A registered-module miss configures in
  a fresh `BuilderSession` S, runs the provider in a child session C, imports nested
  results into C with `GraphBuilder::embed`, copies C into S with
  `GraphBuilder::embed_child`, then copies S into the caller with `embed`. In
  `tree_parent_miss` the complete tree is copied three times; one complete copy costs
  about the `tree_warm` figure. A node at nesting depth d is copied about twice per
  enclosing miss level. Front 12's in-place finalization removes the per-level
  `embed_child` and caller-import copies; the cacheable-miss snapshot remains one copy
  per cacheable scope.
- *Quadratic per-node message-path bookkeeping within one session.*
  `iv_builder_append_node` calls `record_builder_node_message_path`, and
  `configure_concrete_node` calls `attach_builder_node_message_path`; both run a linear
  `find_if` over `BuilderSession::node_message_paths`, which gains one entry per
  concrete node. `builder_direct` therefore costs more per node than leaves placed
  through `NodeInstances`, whose leaves are each configured in a short-lived session.
  Because Front 12 constructs a whole project in one session, this table becomes a
  session-wide quadratic cost. Front 12 changes its representation to a table indexed
  by `NodeBundleHandle`, which is also truncated naturally by scope rollback.

**Implementation handoff record (opened 2026-11-05; remove or move to
`docs/historical/` when Front 12 closes).** Feature: the complete scoped
`BuilderSession` migration in
[scoped_graph_builder_and_subgraph_closure_direction.md](./scoped_graph_builder_and_subgraph_closure_direction.md).
Affected layers: `graph/builder/*` (refs, `GraphBuilderState`, component tables,
importer), `module/builder_session.*`, `module/package_definitions.cpp`,
`runtime/node_instances.*`, `runtime/configuration_expression.cpp`
(`node_reference` materialization left by Front 9), `runtime/graph_connections.cpp`,
`runtime/project_graph.cpp`, the Clang source-introspection plugin (only if
`g.subgraph` scopes become semantic scopes; see F12-2), and tests.

Internal milestones, bottom-up. **Status (2026-11-06):** milestones 1-12 below are
implemented; the repository owner's build succeeded and the complete test suite
passed, both before and after the F12-8 design change. Adversarial tests then found
two defects (F12-10, F12-11); with their fixes, the full suite including the new tests
passed on 2026-11-06. The benchmark rerun showed two regressions (F12-9). A profile
attributed them to exact-size reserves in the bundle importers; after the fix, a
pinned full rerun is faster than the baseline in every scenario. The accepted
design change F12-14 then replaced rollback with failure of the whole configuration
pass; it passed the full suite on 2026-11-06. Throw-site stack capture (under
F12-14) is implemented in source and awaits the next run. The `node_reference`
extension noted under F12-7 is deferred until the project configuration language
is settled (see the co-author's 2026-11-06 reconsideration of the expression
language).

1. Session-owned ref identity: `NodeRef`, `SamplePortRef`, and `EventPortRef` carry
   `details::BuilderSession*`; `GraphBuilderState::_facade`, `bind()`, and `facade()`
   are removed; ref operations become session operations.
2. `GraphBuilder` and `SubgraphBuilder` become non-owning session/scope views; only the
   root view owns its session. Interface operations act on the view's scope; node and
   connection creation act on the session's current scope; stale views fail.
3. Session-owned LIFO `ScopeFrame` stack with real checkpoints and rollback.
4. Scope-aware virtual identity, identical for live finalization and frozen import.
5. `SubgraphClosure` and one `GraphBuilder::subgraph(SubgraphClosure)` surface with
   separate direct paths for `ConfiguredGraph`, live `GraphBuilder`, and move-only
   erased callables.
6. Non-destructive, closedness-checked `GraphBuilder::configured_graph()` snapshot.
7. Constructibility/retainability/cache-keyability split in `NodeInstances`.
8. Registered-module cache misses construct as nested scopes of the caller's session;
   `NodeInstances::ConfigurationContext::semantic_scopes` folds into scope frames.
9. Cache population by snapshot of successful self-contained cacheable scopes.
10. Cache hits import directly as one subgraph scope; hit/miss equivalence tests.
11. `node_reference` configuration-expression materialization into live `NodeRef`s.
12. Removal of `iv_builder_child_session_create`, `GraphBuilder::embed_child`, and the
    live-child importer path; tests; benchmark rerun against the baseline above.

Any compiler error or test failure in the next run belongs to throw-site stack
capture.

Observations and decisions (implementation status, not normative changes):

- *F12-1, rollback cannot be truncation alone (superseded by F12-14: rollback and its
  journal were removed).* `GraphBuilderVirtualNodes::get_or_create`
  returns existing records; `attach_member`, `annotate_input_source_info`,
  `GraphBuilderAnnotations::annotate_node_source_info`, `NodeBundle::apply_ttl`,
  registered-type identity, inverse `virtual_node_handles()`, and boundary port appends
  mutate records created before an inner checkpoint. Connections and the sample/event
  expression tables are append-only. Decision: a checkpoint records table sizes plus a
  journal of in-place mutations of pre-checkpoint records; rollback truncates and
  replays the journal backwards.
- *F12-2, `g.subgraph` scopes need no change (resolved 2026-11-05).* The Clang plugin
  prepares a source identity before the call only for `g.node`; a `g.subgraph` result
  is annotated after its statement. That is sufficient: per
  [graph_builder_embedding_and_matchers.md](./graph_builder_embedding_and_matchers.md),
  an annotated subgraph is the backing virtual node of its scope, and
  `resolve_project_node_path()` already resolves `voice/[i]/osc/[0]` by direct
  scope membership, whether or not the `osc` record spans sibling subgraphs. An
  earlier draft of this record misread the internal message-path claim as the project
  address and proposed a change; the human co-author rejected it. For hit/miss
  equivalence, only scopes that are configured independently today (registered-module
  invocations, including tiled members) need distinct virtual records.
- *F12-3, scoped virtual-record IDs.* `GraphBuilderVirtualNodes::import_child` suffixes
  `"@scope:<root_scope handle>"`, and nested suffixes keep child-local handle numbers.
  Live in-place finalization would produce session-absolute numbers, so hit and miss IDs
  would differ and shift with unrelated project edits. Decision: qualify by semantic
  scope path, which is the normative identity formula; runtime node ID strings change
  format. No document or test depends on the old suffix.
- *F12-4, `NodeInstancePlacement::embedding`.* A same-session miss has no local-to-root
  translation. No consumer outside `NodeInstances` reads the field (path resolution uses
  `NodeInstancePlacement::root`; `ProjectGraph` reads `configured->message_handlers`
  and pins lifetimes). Decision: remove it.
- *F12-5, definition-ID cycle detection.* `ConfigurationStackEntry` and
  `begin_builder_module` reject any definition that configures itself, regardless of
  arguments. Unchanged by this front; recorded because it becomes scope-frame state.
- *F12-6, no synthetic wrapper per registered invocation (decision, 2026-11-05).*
  Before this front, `configure_cached` imported every registered result, leaf or
  module, with `GraphBuilder::embed(..., "Cached node instance")`, which appends a
  wrapper subgraph; nested modules were additionally wrapped by `embed_child`. A node
  created by `g.node` inside module `M` therefore had the wrapper, not `M`, as its
  immediate parent subgraph, and `resolve_project_node_path()` could not reach it
  from `M` (an untested defect; only empty-path project connections were tested). A
  registered invocation now contributes exactly its provider's result to the caller's
  current scope: a leaf's concrete or tiled bundle, a module's own definition scope,
  or a tiled module's member scopes plus the tiled bundle. A cache entry's
  `ConfiguredGraph` holds those contents after an empty root boundary, and a hit
  places them with `GraphBuilderState::import_contents()`, without a wrapper. Root
  graphs built by `ProjectGraph` consequently no longer contain
  `"Cached node instance"`/`"Project node instance"` subgraphs.
- *F12-7, `node_reference` materialization scope.* A direct `NodeRef`/`TypedNodeRef`
  construction parameter is materialized against the root session's live tables by
  `NodeInstances::configure_and_embed()`, which realizes a referencing request after
  the request it references and diagnoses reference cycles. A `NodeRef` nested in an
  aggregate or container parameter is still diagnosed before provider invocation;
  supporting it needs schema-driven construction of `NodeRef` elements. Any argument
  of `node_reference` kind is neither retained nor cache-keyed.
- *F12-8, a nested live `GraphBuilder` is not a `SubgraphClosure` alternative
  (accepted design change, 2026-11-06).* While a `GraphBuilder::nested()` scope is
  open it is the session's current scope, so it cannot be passed into another node's
  construction; it can only be finalized where it is open. In-place finalization is
  now `GraphBuilder::subgraph(GraphBuilder&&)`, and `SubgraphClosure` holds a frozen
  `ConfiguredGraph` (borrowed, or an explicit `std::shared_ptr`) or a move-only
  callable. The normative section was rewritten; the superseded text is preserved in
  [historical/front_12_subgraph_closure_live_builder_alternative_2026-11-06.md](./historical/front_12_subgraph_closure_live_builder_alternative_2026-11-06.md).
  The Clang metadata plugin now classifies an `iv::SubgraphClosure` construction
  parameter as `unsupported` (no configuration-expression form) instead of letting it
  fall through to `converted_scalar`. Verified by the repository owner's full suite on
  2026-11-06. Supplying a `SubgraphClosure` from project configuration was discussed
  afterwards and explicitly postponed by the human co-author; the limitation stays.
- *F12-9, benchmark rerun (friction, unexplained).* The repository owner reran
  `iv_graph_configuration_benchmark` on 2026-11-06 after F12-8, same default shape.
  Bundle counts are not comparable with the baseline: F12-6 removed the per-invocation
  wrapper subgraphs (and `embed_child` wrappers), so the same requests now produce far
  fewer bundles. Medians, microseconds:

  | Scenario | Bundles (baseline → now) | Configure (baseline → now) | Change |
  | --- | ---: | ---: | ---: |
  | `builder_direct` | 2002 → 2002 | 2749 → 1691 | −38% |
  | `tree_cold` | 2318 → 800 | 2563 → 1527 | −40% |
  | `tree_warm` | 2318 → 800 | 317 → 154 | −51% |
  | `tree_parent_miss` | 2318 → 800 | 1713 → 844 | −51% |
  | `tree_uncached` | 2318 → 800 | 4271 → 1195 | −72% |
  | `subgraphs_cold` | 1608 → 580 | 1344 → 1540 | **+15%** |
  | `tiled_cold` | 4634 → 1600 | 4144 → 2281 | −45% |
  | `tiled_warm` | 4634 → 1600 | 655 → 321 | −51% |
  | `instances_warm` | 14657 → 5057 | 3149 → 16120 | **+412%** |

  `instances_warm` is 64 top-level hits of one cached entry into one root session.
  `tree_warm` places 800 bundles with one hit in 154 µs, so 64 hits of about 79 bundles
  each should cost about 1 ms; they cost about 250 µs per request instead. On the hit
  path, `NodeInstances::configure_cached()` → `import_builder_invocation()` →
  `GraphBuilderState::import_contents()` does work linear in the imported graph, plus
  `begin_builder_transaction()` copies of the current semantic frame's declaration
  counts.

  **Diagnosis (2026-11-06, from the repository owner's scaling runs and `perf`
  profile):** the cost is quadratic in the number of hits imported into one session.
  32, 64, and 128 instances took 7.2, 16, and 135 ms; `--padding-definitions 0` did
  not reduce it. About 70% of cycles are in
  `std::vector<NodeBundle>::reserve` called from
  `GraphBuilderNodeBundles::import_contents()`, mostly moving `ConcreteNodeBundle`
  values. `import_contents()` and `import_child()` reserved exactly `size + n`,
  which defeats geometric growth: every import reallocated the session's bundle table
  and moved every existing bundle. A `ConcreteNodeBundle` move is expensive because
  the type holds `NodePorts`, `NodePortConstraints`, and `ReflectedNodeOperations` by
  value. Front 12 made this quadratic by importing every cache hit into the one
  session that holds the whole construction. Before, a nested hit was imported into
  its own small child session. The same mechanism plausibly explains
  `subgraphs_cold`, whose module makes many leaf calls that are cache hits. Fix in
  source: both importers call `GraphBuilderNodeBundles::reserve_appended()`, which
  grows capacity geometrically.

  **Post-fix measurement (2026-11-06, repository owner):** `instances_warm` with 32,
  64, and 128 instances took 1.26, 2.30, and 5.54 ms (minimum) and 4.35, 2.71 (200
  repetitions), and 12.0 ms (median), against 7.2, 16, and 135 ms before the fix and
  3.15 ms for 64 instances at the pre-Front-12 baseline. The quadratic term is gone.
  The 30-repetition medians are bimodal relative to their minimums, which suggests
  the process migrates between core types; pinned runs are needed for precise
  comparison. In the post-fix profile, about 15% of cycles remain in `ConcreteNodeBundle`
  moves from the amortized geometric reallocations and 3% in the copies that a hit
  makes from the cached snapshot. `~ConfiguredGraph` (15%) runs outside the timed
  region. Making a `NodeBundle` move cheap, for example by not holding
  `ReflectedNodeOperations` and the port tables inline, is a Front 16 candidate, not
  a Front 12 requirement.

  **Pinned full rerun (2026-11-06, repository owner, `taskset -c 0`, 200
  repetitions), medians in microseconds, baseline → now:** `builder_direct` 2749 →
  1675; `tree_cold` 2563 → 888; `tree_warm` 317 → 152; `tree_parent_miss` 1713 → 1053
  (minimum 560); `tree_uncached` 4271 → 928; `subgraphs_cold` 1344 → 800;
  `tiled_cold` 4144 → 1383; `tiled_warm` 655 → 346; `instances_warm` 3149 → 1474.
  Every scenario is faster than the pre-Front-12 baseline; F12-9 is closed.
  `tree_parent_miss` adds one cache entry per repetition (215 entries at the end). Its
  median against its minimum shows cache lookup scaling with cache size:
  `configure_cached()` copies `configuration_cache_` and runs
  `configured_definition_closure_matches()` on every candidate before comparing
  `argument_hash`. Indexing by key hash is a Front 16 candidate.
- *F12-10, a refused callable-subgraph finalization left its scope open (defect,
  fixed and verified 2026-11-06; dissolved by F12-14, where a refused finalization
  fails the session).* The template `GraphBuilder::subgraph(Fn&&)` called
  `iv_builder_finish_subgraph_scope()` outside its try block. `finish_builder_scope()`
  refuses to finalize a scope that is not innermost before entering its own rollback
  path. A closure that left a nested view open therefore threw with its scope and the
  nested scope both still open and current: later nodes went into the dead scope, and
  `finish()` failed because a transaction was open. Finalization is now inside the
  try block, so the refusal rolls back the whole invocation. Test:
  `NestedViewLeakedFromAClosureFailsAndRollsBackTheClosure`.
- *F12-11, a caught registered-call failure kept its declaration claim (defect, fixed
  and verified 2026-11-06; dissolved by F12-14, where a caught failure cannot resume
  construction, so no later path can be shifted).* `configure_package_definition_impl()` ran
  `record_builder_module_message_local_ref()` and
  `claim_builder_registered_call_path()` before the resolver opened the invocation
  transaction. When module code caught a failed call, the declaration's member counter
  stayed advanced, so the next call of that declaration got a shifted relative path,
  invocation qualifier, and message-handler path. Rollback is required to restore
  every scope-local table. The claim and the call now run in one session transaction;
  the statement's prepared source identity is consumed before it, so a rollback does
  not hand a stale identity to the next node. Test:
  `RecoveredNestedFailureLeavesNoTraceInTheModule`. The same pattern remains in
  `claim_builder_concrete_node_paths()` (`iv_builder_append_node` and
  `iv_builder_append_tiled_node`): a concrete node whose replacement resolution or
  materialization throws after its claim, caught by module code, still shifts later
  paths. It is not fixed, because a transaction per concrete node has a measurable
  cost; a narrower claim-restore is the likely representation.
- *F12-12, a ref escaping a rolled-back transaction aliases later nodes (dissolved by
  F12-14: nothing is truncated, so no handle is reused).* `NodeRef` is (session, `NodeBundleHandle`), and rollback truncates the
  bundle table. If user code keeps a ref created inside a failed subgraph or call and
  catches the exception, the next node created takes the same handle, and the kept ref
  silently denotes it. The direction document requires that refs from different
  sessions are incompatible and that stale views fail, but it says nothing about refs
  into rolled-back state. Detecting them would need a generation per handle or per
  transaction.
- *F12-13, explicit snapshot closedness versus invocation snapshot closedness
  (decision, 2026-11-06; settled by F12-14 as the structural rule for both paths,
  and the journal check described below was removed).* `GraphBuilderState::snapshot_contents()` (cache
  population) rejected a scope whose transaction journal mutated an older record,
  bundle, or frame; `GraphBuilderState::snapshot_scope()`
  (`GraphBuilder::configured_graph()`) checked only connections and virtual records.
  The publicly reachable difference is an operation on enclosing state while the scope
  is current. Examples are an interface operation on an enclosing view, such as
  `root.input<"x">()` while `root.nested()` is open, or `ttl(...)` on an enclosing
  node. The operation belongs to the open scope's transaction, since abandoning the
  scope undoes it, but a closed copy of the scope cannot reproduce it. The human
  co-author decided that the two snapshot paths must not diverge, and tentatively
  chose refusal, pending a worked example. `snapshot_scope()` now applies
  `journal_since()` to the scope's own checkpoint and reports
  `enclosing_scope_mutation`. Test:
  `ScopeWhoseTransactionChangedAnEnclosingScopeIsNotSnapshotable`.

- *F12-14, rollback replaced by failure of the whole configuration pass (accepted
  design change, 2026-11-06; implemented and verified by the repository owner's full
  test suite on 2026-11-06).* The human co-author
  decided that a construction failure, and any invalid project node configuration,
  stops the configuration pass instead of being rolled back. The normative text is
  "Construction failure stops the configuration pass" in
  [scoped_graph_builder_and_subgraph_closure_direction.md](./scoped_graph_builder_and_subgraph_closure_direction.md);
  the superseded contract is in
  [historical/front_12_transactional_rollback_2026-11-06.md](./historical/front_12_transactional_rollback_2026-11-06.md).
  This supersedes F12-1. It dissolves F12-10, F12-11, and F12-12, and it settles
  F12-13 as the structural rule, which reverts the journal check added to
  `snapshot_scope()`. Implementation plan, in order:
  1. `GraphBuilderState`: remove the journal, journal serials, checkpoints, and
     `rollback_checkpoint`/`truncate`/`restore`. An invocation keeps only the table
     sizes it needs for `snapshot_contents()`. Both snapshot paths check
     closedness structurally.
  2. `BuilderSession`: remove the transaction stack and `SemanticFrameMark`. Add the
     first-failure record, refuse construction operations once it is set, and make
     scope or invocation abandonment record a failure.
  3. Record failures at the library boundaries that run authored code: module
     bodies, leaf factories, closure invocations, registered calls, and
     concrete-node replacement resolution.
  4. `NodeInstances::configure_and_embed()`: check every request before
     construction and fail the batch on any problem; otherwise stop at the first
     construction failure.
  5. `GraphConnections`: a connection that resolves to no ports or to incompatible
     ports stays a diagnostic. An exception while applying a resolved, compatible
     connection sets `GraphConnectionsBatchResult::failure` and fails the pass.
  6. `ProjectGraph::rebuild_locked()`: on failure keep `current_generation_`,
     retain the failure diagnostics for the desired revision, and make replacement
     previews fail.
  7. Tests: remove the rollback and caught-failure tests and add pass-failure and
     refusal tests.
  Throw-site stacks (implemented in source 2026-11-06, awaiting the next run):
  `throw_site_hook.cpp` interposes the Itanium ABI's `__cxa_throw` in
  `libiv_builder`. Every C++ throw in the process records its stack, without
  symbolization, in a small per-thread ring (`throw_site.h`) before unwinding
  begins. `record_builder_failure()` uses that stack when the failure is a
  `std::exception` still in the ring. The stack therefore starts at the throw,
  in library or authored code, rather than at the handler, whose frames above
  the catch were already unwound. Otherwise it falls back to the handler's stack.
  The process terminate handler uses the same lookup, so lifecycle crash logs
  now show where the exception was thrown, not `invoke_node_lifecycle()`'s
  catch.

  Implemented representation: `BuilderSession::failure` (message, context lines,
  `std::stacktrace`) is set by `record_builder_failure()`, and
  `session_state()` refuses every construction operation once it is set. These
  boundaries record what escapes them:
  - `configure_package_definition_provider()` for every leaf and module factory;
  - `configure_package_definition_impl()` for registered calls;
  - the callable `GraphBuilder::subgraph(Fn&&)` for closures;
  - `configure_concrete_node()` and its variants for node constructors;
  - `NodeInstances::configure_and_embed()` for top-level requests.

  `throw_if_builder_failed()` runs after authored code returns normally, so a
  swallowed failure still fails the invocation. `abandon_builder_scope()`
  records a nested view that was dropped without finalization; while an
  exception is unwinding it records nothing, because the boundary records the
  exception itself. `GraphBuilderState` keeps `GraphBuilderMark` (table sizes)
  for invocation snapshots and no longer journals or truncates.
  `ProjectGraph::configuration_failure()` exposes the latest failed pass, and
  `ProjectGraphGeneration::diagnostics` is removed because a published
  generation has none. A failed pass is written to the server diagnostic log.
  When the `NodeInstances` batch fails, `ProjectGraph` still delivers the
  connection mutation to `GraphConnections`. A request without a root builder
  only commits it to the desired connection set, so the desired revision stays
  complete. The first run showed this was missing: a project loaded before its
  definitions lost its recorded connections. `ProjectGraph::manual_controls()`
  exposes the desired manual controls, matching
  `NodeInstances::desired_instances()` and `GraphConnections::desired_connections()`.

  Every exported session operation is also a failure boundary: the
  `details::iv_builder_*` functions and the `GraphBuilder` members in
  `graph/builder/builder.cpp`, and the `NodeRef`, `SamplePortRef`, and
  `EventPortRef` members in `graph/builder/node_refs.cpp`. Each is a
  function-try-block that records the escaping exception on that operation's
  session and rethrows. So an error from a module's own direct call, for example
  a wrong port name, fails the session even if the module catches it. This also
  holds for host code, so a refused `finish()`, a refused scope finalization, and
  embedding an unfinished graph fail the session; the tests that expected the
  session to stay usable were updated. The recorded stack is captured in the
  innermost operation's handler, so it shows the failing operation and the
  authored call chain that reached it.

Implemented representation (implementation status):

- `NodeRef::_session`, `SamplePortRef::session`, and `EventPortRef::session` carry the
  owning `details::BuilderSession`; ref operations are the `details::iv_builder_*`
  functions in `graph/builder/session_ops.h`. `GraphBuilder` is a session view plus a
  `details::BuilderScopeId`; only the default-constructed root view owns its session,
  and a view returned by `GraphBuilder::nested()` owns its open scope until
  `subgraph(...)` finalizes it or its destructor rolls it back.
- `GraphBuilderState` owns the LIFO `ScopeFrame` stack (root, definition, subgraph),
  `GraphBuilderCheckpoint`s, and a journal of pre-transaction copies of records,
  bundles, and frame ports mutated in place. Root and definition scopes own a
  virtual-record namespace, indexed by namespace, source identity, and type identity;
  subgraph scopes share their definition's namespace.
- `BuilderSession` owns the transaction stack that additionally restores message
  handlers, node-configuration allocations, package-use marks, the module stack,
  semantic frames, declaration counters, and node message paths (now a table indexed
  by `NodeBundleHandle`). Registered-call path counting moved from
  `NodeInstances::ConfigurationContext` into the session's semantic frames; the
  resolver receives the call's relative path.
- A registered invocation's records are qualified with
  `builder_invocation_qualifier()` of its relative path (`"@&<instance>"` at the
  project root) after any snapshot; frozen embeddings and nested live scopes are
  qualified by their subgraph handle relative to the enclosing namespace.
- `NodeInstances::configure_cached()` constructs a miss in the caller's session as one
  `details::BuilderInvocation`, snapshots it only when its arguments are cache-keyable
  and its contents are closed, and imports hits. Arguments that are constructible but
  not retainable are passed through borrowed and make the invocation unreusable.
  `NodeInstancePlacement::embedding` is removed (F12-4).
- New tests: `tests/scoped_graph_builder_gtest.cpp` and the Front 12 cases appended to
  `tests/modules/node_instances_configuration_gtest.cpp`, including a closure passed as
  a registered-module construction argument and invoked repeatedly in a deeper module.

Front 9 must determine whether a project-node construction argument needs a live graph
handle across `BuilderSession` objects. If it does, this front owns the safe same-session
materialization path. Front 9 must retain the stable semantic reference until this path
exists.

## Front 13: source introspection and semantic node interaction

**Primary ownership:** the typed semantic event/control layer inside the backend.

This front owns:

- making source introspection mandatory package infrastructure where required;
- validation and stable identity for declared `State`/`TockState` fields;
- member-pointer-based declaration validation;
- typed node-message handler discovery;
- generated event serializers/deserializers and language-neutral event schemas;
- stable `NodeAddress` and selector semantics;
- leaf and module-node handlers;
- ordered one-way node-to-node event dispatch;
- graph-revision-safe semantic addressing;
- node-local replace/reconfigure operations;
- accepting the successor definition and its complete typed construction arguments
  from a replacement handler;
- converting those supplied arguments to durable Front 9 values before package-owned
  C++ objects can expire;
- interaction journaling/commit integration without exposing realtime storage to the
  public handler context.

A successful node-local replacement is durable canonical project state keyed by stable
`NodeAddress`. It stores the successor definition and complete structured configuration,
is reapplied by `NodeInstances` during every later reconstruction, and participates in
`ProjectPersistence` save/load. It is not a temporary overlay on one `ConfiguredGraph`,
and successive replacements replace the prior complete record rather than accumulating
configuration modifiers.

The replacement handler owns preservation of unchanged values. A leaf-node handler
can read them through `this`. A class-module handler can read them through `this`.
Front 13 does
not recover predecessor arguments from `ConfiguredNodeInstance`.

Front 9 supplies the durable value representation and reconstruction. Front 13 owns
typed successor argument submission, event semantics, and replacement
transactionality. It does not own the configuration grammar.

The live-observation and presentation telemetry API remains separate and unresolved.
Front 14 does not own it.

**Backend implementation status (2026-11-04): complete and verified.** The repository
owner's full 747-test suite passes. The source implements compiler-discovered leaf handlers,
compiler-discovered class-module handlers, stable `NodeAddress` dispatch, ordered send/replace
execution, one delivery per concrete node in one root interaction, complete typed
successor arguments, durable replacement records, reconstruction after later graph
builds, project save/load, and the serialized `nodeMessages.dispatch` transport. A
replace followed by a send to the successor at the same stable address is treated as
two different concrete nodes. A replacement of an ancestor similarly creates a new
concrete descendant world, while an unrelated replacement does not permit a second
delivery to an unchanged node.

**Class-module decision (2026-11-03):** a module that receives semantic messages is
registered with `IV_MODULE_CLASS`. Its constructor receives configuration arguments,
`build_graph(GraphBuilder&) const` creates its graph, and compiler-discovered public
const `on_message(ModuleMessageContext<Module>&, Event const&)` overloads receive
events. `ctx.ref<"local">()` maps an authored local name from `build_graph()` to its
compiler-generated `ProjectVirtualNodeSelector`; `.member(n)` and
`.tiled_child<"name">()` append the existing selector types. The configured handler
retains the class instance and semantic selector map. It never retains a
builder-local `NodeRef` or `NodeBundleHandle`. `IV_MODULE` remains the handler-free
function form.

This makes both leaf and class-module handler sets package metadata. Each finalized
package emits a language-neutral `.node-events.json` schema beside its `.ivpkg.bc`
artifact. The sidecar maps registered leaf/module definition IDs to their supported
event types without exposing `NodeCodeKey`. `nodeMessages.getProtocol` combines the
event types retained by the accepted package revisions with the exact event types
supported by each active `NodeAddress` in the current `ProjectGraphGeneration`.
Its `projectGraphGeneration` and `definitionsGeneration` fields define the snapshot
lifetime.

The backend does not generate TypeScript, Vue components, or another frontend-language
API. A frontend adapter or supplementary plugin consumes the neutral package/runtime
schemas and owns its language-specific types, codecs, event factories, naming rules,
and imports. This permits the Vue frontend to be replaced by a C++/Vulkan frontend
without changing the semantic backend protocol. The neutral schema excludes
`NodeCodeKey`, LLVM symbols, function pointers, storage offsets, and other backend ABI
facts. The nominal ID plus definition fingerprint remains the event compatibility
identity; `displayName` is diagnostic only.

## Front 14: presentation and persistent manual controls

**Primary ownership:** semantic presentation definitions and user-controlled dynamic
inputs.

**Accepted scheduling simplification (2026-11-04):** Front 14 uses predictive
compilation, demand-driven activation, one current replacement request, and a
debounced return to constant specialization. It does not use a variant cache or a
generic execution path during compilation.

**Source implementation status (2026-11-04):** the persistent manual-control and
GraphJit specialization portion is implemented and awaits the repository owner's
build and test run. It includes stable `ManualControlId` resolution, project
persistence, socket RPC with server-owned client identities, disconnect cleanup,
participation through the existing Sequential combination callback, callback-wide
scrub interpolation, predictive/required compilation, stale-result rejection,
debounced constant specialization, and compatible code-only activation that retains
`NodeStorage`. The initial authored value surface supports scalar `Sample` Sequential
Tick inputs; this is an implementation limit rather than a general presentation
contract.

The presentation registration and selection portion is implemented in source and
awaits the repository owner's build and test run. `IV_NODE_PRESENTATION` records only
the target definition ID, a nominal presentation ID, and a diagnostic display name.
The package finalizer and loader preserve this record. `PackageRevision`,
`IvPackageInfo`, and socket RPC publish it without a frontend component name.

The VS Code frontend owns `NodePresentationRegistry`. The registry intersects package
descriptors with installed frontend providers and uses
`frameworkDefaultNodePresentationProvider` when no provider matches. The display mode
is one of `compact`, `nano`, `mini`, or `editable`. The backend does not select the
mode or import a frontend implementation.

This front owns:

- `NodePresentation` and framework default presentation;
- global display modes;
- authored/scoped semantic rebinding;
- presentation lifetime independent of frontend component lifetime;
- persistent manual values;
- participation policy;
- manual values as Tick/ephemeral inputs;
- scrub-only interpolation;
- dynamic-input sets;
- predictive hover compilation without activation on hover;
- one active executable graph and one logically current replacement request;
- demand-driven dynamic activation after compilation;
- latest-request-wins scheduling and stale-result rejection;
- an explicit idle/debounce policy for return to constant specialization;
- compatible code-only activation with no state reconstruction.

Front 14 does not own the undefined live-observation API. Its capability,
subscription, representation, and scheduling questions remain future design work.
Front 14 also does not add a cache of old control specializations. The prior policy is
preserved in
[historical documentation](./historical/front_14_manual_control_jit_policy_before_demand_activation_2026-11-04.md).

Compilation below approximately 200 ms is a target from an early proof of concept. It
is not a guaranteed latency. Front 16 can add reuse or more scheduling machinery after
measurements show a benefit.

**Dependencies:** stable `NodeAddress` and selector semantics from Front 13, revision
continuity from Front 6, durable project mutations from Front 10, and graph activation
from Front 8. Front 12 can improve later scoped construction but does not block Front
14.

## Front 15: RPC route layer and multi-client project session server

**Primary ownership:** transport/session infrastructure around typed application APIs.

This front owns:

- replacing hard-coded socket dispatch with a route/event registry where still needed;
- multiple simultaneous clients;
- per-client outbound queues/backpressure;
- ordered mutation execution;
- snapshots and monotonically increasing project revisions;
- subscription/replay;
- expected-revision mutation requests;
- reconnect/resynchronization;
- local server discovery/election;
- transport-independent endpoint semantics;
- client identities/capabilities;
- optional remote-audio lease/WebRTC relay work.

This front consumes project mutations from front 10 and semantic node events from front
13. It should not invent domain-specific graph semantics in the transport layer.

Implementation status (2026-11-04): the core project-session work is complete in
source and verified. The repository owner's complete C++ build passed all 747 tests.
The VS Code client typecheck and npm tests also pass.
`SocketRpcRouteRegistry` owns route metadata and typed event dispatch.
`SocketRpcServer` owns multiple clients, bounded per-client output queues, ordered
commands and notifications, revisions, expected-revision rejection, replay, topic
subscriptions, snapshots, local Unix-socket election, and local capabilities. The VS
Code client discovers or starts the project session, reconnects with replay or a
snapshot, and does not stop the server when the client exits.

`ProjectPersistence::snapshot()` supplies the durable project records in a session
snapshot. The route also reads the current package, node-instance, audio-device,
autosave, and node-message records through their typed application events. The
inherited `--rpc-fd` path remains as a trusted compatibility and test transport.

The C++ session-server tests cover multiple clients, ordered revision publication,
stale-mutation rejection, topic-filtered replay, bounded-output backpressure, local
authentication/election, and complete `session.snapshot` aggregation through the
typed application events.

The optional remote-audio lease and WebRTC relay remain unimplemented. This optional
media work does not block completion of the core multi-client session server.

## Front 16: measurement-driven optimization and optional PGO

**Primary ownership:** optimization after the correctness architecture is stable.

Candidate work includes:

- batch-size/cost modeling beyond front 2's correctness-capable scheduler;
- SIMD profitability heuristics;
- callback/kernel fusion;
- storage-layout co-optimization for batched nodes, including per-lane
  contiguous-input scratch so that block callbacks with continuous inputs can batch
  (Front 2 friction item 2);
- alias/materialization cost models;
- persisted-recording backing-page reclamation and measured compaction after old
  `PersistedDataStore::Snapshot` pins release;
- persistent/segmented publication metadata for recordings with very many disconnected
  canonical Regions, if profiling shows that rebuilding the small Region descriptor
  vector dominates changes to one Region;
- in-place event fan-in over a transient source whose last read is the merge;
- transient storage reuse/liveness improvements;
- immutable-value specialization;
- finer NodeInstances cache invalidation/reuse;
- `NodeInstances::configure_cached()` cache lookup that does not scale with cache
  size: today it copies `configuration_cache_` and runs
  `configured_definition_closure_matches()` on every candidate before comparing
  `argument_hash`, which `tree_parent_miss` exposes once the cache holds hundreds of
  entries (median 1053 µs against a 560 µs minimum). Index by key hash and check the
  definition closure only for hash-equal candidates. Source: Front 12 F12-9 pinned
  rerun;
- cheap `NodeBundle` moves: `ConcreteNodeBundle` holds `NodePorts`,
  `NodePortConstraints`, and `ReflectedNodeOperations` by value, so the amortized
  geometric reallocations of the session bundle table remain about 15% of
  `instances_warm` after the F12-9 fix. A cache hit's copies from the cached snapshot
  are a further 3%. Source: Front 12 F12-9 post-fix profile;
- measured builder/package reload optimization;
- runtime PGO experiments if profiling demonstrates a worthwhile target;
- a lock-free SPSC capture queue inside `AudioInputSynchronizer`, if profiling shows
  that the current non-blocking `try_lock()` path drops too many input slices under
  capture-thread contention.
- running every contextually replayable Tick node only in background replay,
  removing it from the realtime root and giving its live Sequential consumers
  playback delivery (moved from Front 4 step B2, 2026-10-23). This depends on
  advance materialization demand, so that background produces the replayed values
  before the playhead reaches them.

The event fan-in item is not implemented in Front 7. `EventOutputPortId` and
`EventProducerGroupPlan` must remain separate logical representations. Front 16 can
let two transient `EventRepresentationPlan` records use one allocation when the
source ends at its `EventMergePlan` and the target begins there.

The proof must show that all other source reads finish before the merge. Both
representations must be transient. The group cannot restore retained history or
accumulate events across SCC slices. The callback must still see its own logical
capacity when the shared allocation has the larger group capacity.

A group-private per-source conversion result is the preferred in-place source. A raw
callback sequence is legal only when its raw view is dead at the merge. If a common
post-merge conversion can replace the group representation, fusion is a separate
candidate and can cost less. The cost model must compare the separate, fused, and
in-place forms.

`EventMergePlan` will need an explicit in-place source index. The transient allocator
must permit this one proven overlap. The merge helper must preserve semantic source
order and the optional source-index sidecar. This optimization must not restore the
old `producer_home_source_index` ownership model.

Node-facing Tick port capabilities are another candidate for this front. The complete
legal API remains enabled by default. The intended explicit configuration must make
both common cases concise: opt out of individual capabilities, or disable all
capabilities and enable only a short named set, most likely through a typed bit mask
and an `only(...)`-style helper. Exact names and grouping remain deliberately
unsettled until the generated structures and measured costs are stable.

Capability inference and explicit declarations may coexist. Inference lets a new node
prototype pay only for operations its compiled callbacks actually use. An explicit
capability set is a contract: detected use outside that set must reject the node or
graph rather than silently re-enable an accidentally used operation. Prefer a C++
source diagnostic when the toolchain can establish the conflict; otherwise GraphJit
must return a precise compilation diagnostic. The inferred capability summary and the
explicit allowed set must remain separate facts so optimization cannot weaken the
author's enforcement contract.

**Proposed design change (2026-10-23; postponed until after Front 2):**
activity-specialized root variants. GraphJit would compile Tick-root variants for a
selected dormant set. A dormant primitive calls its skip operation unconditionally,
and an active primitive calls its Tick operation without the dormancy branch. When a
skipping part of a variant detects that it must stop skipping, the runtime replaces
that variant with one that ticks.

Human co-author direction (2026-10-23), not yet reconciled with the normative
documents:

- Refinement is initiated synchronously between two graph calls. Variant compilation
  runs asynchronously on the compilation worker. A compiled variant is published
  only synchronously between two graph calls.
- The setup code that submits a refinement inspects the residual activity state in
  the allocated `NodeStorage` at that moment. It decides synchronously which subgraphs
  skip and which execute. It then submits the resulting graph refinement profile to
  GraphJit and returns.
- Replacement at the graph-call boundary should reuse the ordered
  `ProjectGraph -> GraphJit -> GraphExecutor::stage()` path as far as possible.
- Dormancy forms a tree. Each subgraph that can be put to sleep on its own contains
  exclusive whole subgraphs that can in turn be put to sleep on their own. The variant
  cache can be structured along that tree. A naive policy such as LRU is a starting
  point; profiling should decide the final policy.

Observed constraints (implementation analysis, 2026-10-23):

- A dormant primitive must still tick in the same block in which its inputs change.
  Those inputs are produced inside the root by upstream primitives, so a variant
  still evaluates the wake condition at that point. Its wake path must handle the
  current block before the boundary replacement takes effect.
- Active primitives still track quiescence through the output-clear scan in
  `emit_primitive_activity_update()`. Variants remove the dormancy branch and the
  dynamic lane compaction in `emit_execution_batch()`. They do not by themselves
  remove the input or output scans.
- The larger saving is a wholly dormant subtree. If skip output is provably constant,
  only inputs at the subtree's boundary with active primitives need a wake check.
- Variants share one `NodeLayout`, so switching between them needs no Front 6 state
  migration.
- Normative reach: the generated Tick root currently owns each primitive's activity
  decision. This proposal shares the dormancy choice with refinement setup code.

The current batch emitter is compatible with this proposal: a variant would supply
compile-time per-lane decisions, and the emitter already folds those into static
spans.

Runtime PGO remains an optional discovery direction rather than a prerequisite for the
architecture.

## Dependency summary

The main execution path is approximately:

```text
                           +--> 2 batched callbacks -----------------+
                           |                                         |
1 port realization --------+--> 3 Region/Coverage ----+              |
       |                   |                          |              |
       |                   +--> 7 GraphJit semantics |              |
       |                                              |              |
       +----------------------> 6 revision continuity|              |
                                                      |              |
existing background -------> 4 background->Tick -----+              |
                                |                     |              |
                                +--> 5 capture        |              |
                                                      v              v
                                           8 application/executor integration
                                                      |
                                                      v
                                               measured baseline
                                                      |
                                                      v
                                         12 scoped builder migration
```

The main control-plane path is approximately:

```text
9 restricted configuration language ---> 10 project persistence/mutations
             |                                      |
             +-----------------> 13 semantic events-+
                                      |
                                      v
                              14 presentation/controls

10 project APIs + 13 semantic events ---> 15 session/transport
```

Front 11 (package provenance/caching) can proceed largely independently. Front 16
should consume measurements from the integrated system rather than setting semantic
requirements for earlier fronts.

## Integration checkpoints

The fronts should integrate through a few explicit checkpoints rather than by waiting
for every neighboring project to finish:

1. **Resolved-port checkpoint:** front 1 publishes a stable realization object; fronts
   2/3/4/6/7 consume it.
2. **Normalized-callback checkpoint:** front 2 publishes unconditional scalar/batch
   compiler operations; GraphJit scheduling no longer inspects authored callback shape.
3. **Random-access checkpoint:** front 3 publishes the canonical Region/Coverage
   ownership/versioning interfaces; fronts 4/5/6 stop depending on fixed-width
   storage assumptions.
4. **Execution checkpoint:** fronts 4/6/7 provide enough correctness for front 8 to make
   `GraphExecutor` the normal application path while retaining separate internal realtime/background actors.
5. **Configuration-value checkpoint:** front 9 publishes the restricted parser plus
   typed/structured value representation; fronts 10/13 share it for persistence and
   replacement.
6. **Semantic-control checkpoint:** fronts 10/13 publish transport-independent typed
   APIs; front 15 provides multi-client/session transport without duplicating domain
   logic.

Temporary adapters are acceptable between checkpoints. Shared ownership of the same
semantic rule is not.
