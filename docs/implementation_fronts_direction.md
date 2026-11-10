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
_(Superseded 2026-11-07 by the authoring-index decision in Front 16: a Tick sample
output files each value at the index it was authored for, so its authored and retained
views both use offset 0.)_

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
_(Superseded 2026-11-07 by the authoring-index decision in Front 16: the carried future
extent belonged to the finalization-index convention. The disconnected-output carry now
uses `future_frames = 0`, and the output owns one `[-(history + latency), 0)` window.)_

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

**Status (2026-11-06): closed.** The repository owner's complete test suite passed
with the final change, throw-site stack capture, included, and the owner closed the
front. The implementation handoff record is preserved in
[historical/front_12_implementation_handoff_2026-11-06.md](./historical/front_12_implementation_handoff_2026-11-06.md).
It holds the milestones, decisions F12-1 to F12-14, friction records, and benchmark
tables.

Delivered representation (implementation status):

- *One session per configuration pass.* `GraphBuilder` and `SubgraphBuilder` are
  non-owning views of one `details::BuilderSession` plus a `details::BuilderScopeId`.
  Refs carry their session (`NodeRef::_session`, `SamplePortRef::session`,
  `EventPortRef::session`), so views of one session are compatible and other
  sessions are rejected. `GraphBuilderState` owns the LIFO `ScopeFrame` stack. Root
  and definition scopes own a virtual-record namespace.
- *Registered invocations.* A cache miss constructs in place in the caller's
  session (`BuilderInvocation`). A cacheable, closed invocation is copied for
  reuse: `GraphBuilderMark` records its table sizes and
  `GraphBuilderState::snapshot_contents()` copies it. A hit imports through
  `GraphBuilderState::import_contents()` without a wrapper subgraph (F12-6). Virtual
  records are qualified by the call's semantic path (F12-3). Constructibility,
  retainability, and cache-keyability are separate properties of an argument.
- *Subgraphs.* `SubgraphClosure` holds a frozen `ConfiguredGraph` (borrowed or
  shared) or a move-only callable. `GraphBuilder::subgraph(GraphBuilder&&)`
  finalizes a nested view in place (F12-8). `GraphBuilder::configured_graph()` is a
  non-destructive snapshot whose closedness is structural.
- *Project-node references.* A direct `NodeRef`/`TypedNodeRef` construction parameter
  is materialized against the root session in `NodeInstances::configure_and_embed()`.
  Requests are realized in reference order, and cycles are diagnosed.
- *Construction failure (F12-14).* There is no rollback. The first failure is
  recorded on the session, and `session_state()` refuses every later construction
  operation. Every exported session operation records what escapes it, and so do
  the boundaries that run authored code. Any failure fails the whole configuration
  pass: `ProjectGraph` keeps the previous generation and exposes
  `configuration_failure()`, and the desired revision is still committed. A failure
  diagnostic carries the stack from where the failure was thrown, through the
  `__cxa_throw` hook in `throw_site_hook.cpp` (`throw_site.h`).
- *Performance.* After F12-9, a pinned rerun of `iv_graph_configuration_benchmark`
  is faster than the pre-Front-12 baseline in every scenario. Examples:
  `instances_warm` 3149 → 1474 µs, `tree_cold` 2563 → 888 µs, `tree_uncached`
  4271 → 928 µs. Two optimization candidates found on the way are listed under
  Front 16.

Deferred by the human co-author (2026-11-06): F12-7, a `NodeRef` nested inside an
aggregate or container construction parameter. It waits until the project
configuration language is settled.

Front 9 must determine whether a project-node construction argument needs a live graph
handle across `BuilderSession` objects. If it does, this front owns the safe same-session
materialization path. Front 9 must retain the stable semantic reference until this path
exists.

_Status (2026-11-06): resolved for direct `NodeRef`/`TypedNodeRef` parameters by the
same-session materialization path above; nested parameters are the deferred F12-7._

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

**Implementation status (2026-11-06): complete and verified.** The repository
owner's complete test suite has passed with Front 14 included; this record had
still said "awaiting a run" from 2026-11-04. The persistent manual-control and
GraphJit specialization portion is implemented. It includes stable `ManualControlId` resolution, project
persistence, socket RPC with server-owned client identities, disconnect cleanup,
participation through the existing Sequential combination callback, callback-wide
scrub interpolation, predictive/required compilation, stale-result rejection,
debounced constant specialization, and compatible code-only activation that retains
`NodeStorage`. The initial authored value surface supports scalar `Sample` Sequential
Tick inputs; this is an implementation limit rather than a general presentation
contract.

The presentation registration and selection portion is implemented and verified. `IV_NODE_PRESENTATION` records only
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

**Priority target (human co-author, 2026-11-06): edit-to-activation latency.** A
user edit to the authored project graph should take at most 0.2 s, and preferably
less, from the edit to the executor accepting the recompiled generation. Edits here
mean node instances, connections, and configuration. Package compilation is
excluded. This target comes before the other Front 16 categories.

**Principle (human co-author, 2026-11-07): edit-invariant work is done before the
compilation starts.** Anything that a graph edit cannot invalidate must already be
computed by the time a `GraphJit` compilation is launched. For each per-compilation
cost, ask what it depends on:
- Work that depends only on a package revision belongs in the package build
  (`iv_package_finalize`).
- Work that depends on a node type and its realization is computed once and reused
  across generations.
- Only work that depends on the edited graph stays in the compilation.

A cache filled lazily by the first compilation that needs it does not meet this
principle, because that compilation pays the cost.

Measurement (implementation status, 2026-11-06): setting `IV_EDIT_TIMING=1` makes
`ProjectGraph::rebuild_locked()` write one "edit timing" entry per published
rebuild to the diagnostic log (`IV_LOG_FILE`):
- configuration time: `NodeInstances`, `GraphConnections`, and root finish plus
  generation assembly;
- `GraphJit` time;
- `GraphExecutor::stage()`.

`GraphJitCompileResult::timings` adds one `GraphJitCompileTimings` record per
compilation: capture, package bitcode parse (with byte count), lowering plan,
transition plan, IR emission and package linking, verification, O3 optimization,
and ORC code generation, plus function and instruction counts.
`ProjectGraphGeneration::rebuild_timings` and `graph_jit_timings` retain the same
numbers on each published generation.

`benchmark/iv_edit_latency_benchmark.cpp` (target `iv_edit_latency_benchmark`)
measures edits without a frontend. It runs the server's pipeline: package watcher,
`PackageJit`, definitions, `NodeInstances`, `GraphConnections`, `ProjectGraph`,
`GraphJit`, and `GraphExecutor`. A realtime thread paced to the block period
activates and ticks generations as the audio callback does. The benchmark builds
a project of built-in nodes in one of several shapes of size N (`--shape`,
`--size`; added 2026-11-07), so that compilation and tick-scheduling changes are
not tuned to one graph:
- `voices`: wide and shallow, identical chains;
- `chain`: one deep series of filters;
- `mixed`: alternating oscillator and filter types, warpers, and cross-voice
  subtraction;
- `feedback`: N instances of `iv.bench.feedback_voice`, a module the benchmark
  writes into its workspace as a project-local package. Its one-sample loop
  `saw -> subtract -> low pass -> detach() -> subtract` forces sample-by-sample
  interleaving across node types. Project connections (`ProjectSampleConnection`)
  cannot mark feedback; only module code breaks a cycle, with `detach()`. The
  first version built the loop from project connections through
  `iv.builtin.latency`, and `GraphJit` rejected every generation with "implicit
  cycle" (2026-11-07).

It then repeats five edits: change a constant, add a node, connect it, disconnect
it, and delete it. It reports per-phase medians for each kind of edit. No registered
built-in node produces events without external MIDI, so an event-heavy shape needs a
test package with an event generator.

Observations from source, before any measurement:
- *Two full compilations per edit.* `GraphJit::handle_project_graph_transaction()`
  runs a steady `compile()` and then a `compile_transition()` whenever a
  predecessor is active. Each repeats every phase below.
- *Package bitcode is re-parsed on every compilation.* `compile_impl()` reads and
  parses each used package's finalized bitcode from disk into a fresh
  `LLVMContext`. Lowering then links the selected callbacks with `LinkOnlyNeeded`.
- *Whole-project optimization and code generation.* `optimize_project_module()`
  removes the packages' `optnone`/`noinline` barriers and runs the O3 per-module
  pipeline over the entire project module. ORC then generates code at
  `CodeGenOptLevel::Aggressive` on one thread. No compiled code is reused between
  generations.

Measured baseline (2026-11-07, repository owner, `iv_edit_latency_benchmark`,
medians per edit): about 14.5 s at 16 voices (81 bundles) and 24.6 s at 64 voices
(321 bundles). Configuration and lowering planning take 1-2 ms. Each of the two
compilations spends about 3.2 s (16 voices) or 6.0 s (64 voices) in O3 and about
3.9 s or 6.1 s in code generation. Package parse, emit, and verify together take
about 0.17 s.

Both sizes import 5,564 functions and about 180,000 instructions; 64 voices adds
only about 12,000. The function inventory (`IV_GRAPH_JIT_PROFILE_DIR`) showed
callbacks for every built-in node type, not only the four the project uses, plus
host-side construction code (`describe_node`, `NodeTypeConfigureAdapter`,
`ErasedConfigurationArguments`).

*Import-closure defect (fixed in source 2026-11-07, awaiting measurement):*
`IV_NODE_COMPILER_RECORD_ATTR` and the configuration type-identity records are
`used`, so they sit in the package's `llvm.used`. `Linker::LinkOnlyNeeded` always
links appending-linkage globals and everything their initializers reference, so
each compilation imported the whole package. Lowering now erases the package's
`llvm.used` and `llvm.compiler.used` before linking. `GraphJit` has already
scanned the records from the parsed package, and the project module needs none of
them.

Measured after the import fix (2026-11-07, 16 voices): about 3.15 s per edit, or
about 1.56 s per compilation. That breaks down as O3 about 0.80 s, code generation
about 0.67 s, package parse about 58 ms, emit about 22 ms, and verify about 8 ms.
The largest function before optimization is the root tick (4,151 instructions);
next come one batch wrapper per node type (about 1,100 each).

*One compilation per edit (implementation decision, 2026-11-07, awaiting
measurement):* with an accepted predecessor,
`GraphJit::handle_project_graph_transaction()` now runs only the cutover
compilation. Its module defines both the transition root and the steady root, and
its steady plan is built first. The separate steady compilation was used only to
check the transition's steady storage extent against an identical plan; its
machine code was discarded. `compile_transition()` remains for direct callers.
*Lazy package bitcode:* `getOwningLazyBitcodeModule` now reads globals eagerly,
and the linker materializes only the callbacks' closures.

Measured after single compilation and lazy bitcode (2026-11-07). Per edit:
- O3, 16 voices: 1.52 s compile; realtime tick 396 µs mean, 626 µs worst, of a
  5,333 µs block.
- O3, 64 voices: 6.51 s; tick 1,523 µs mean.
- O0 with fast instruction selection, 16 voices: 0.16 s; tick 4,064 µs.
- O0 with fast instruction selection, 64 voices: 0.43 s; tick 15,496 µs, which
  misses the block deadline.

So a whole-project O0 build is not viable even as a temporary build. The
function inventory at 64 voices shows that only the root grows with the project:
`__iv_graph_root_tick_block` goes from 4,151 to 16,583 instructions, about 260 per
voice. There are four primitive wrappers, one per batched node type, at both
sizes, and the imported package code does not change. A further profiling knob,
`IV_GRAPH_JIT_ROOT_OPTNONE=1`, leaves only the generated `__iv_graph_root_*`
functions unoptimized and un-inlined. It separates the root's share of compile
time and of realtime cost.

After 16 to 64 voices, compile time rose about 70% while instructions rose about
7%. Superlinear passes on the growing root functions are the likely cause; after
optimization, `__iv_graph_root_tick_block` is the largest function by far (22,501
instructions at 16 voices).

The first `IV_GRAPH_JIT_ROOT_OPTNONE=1` measurement is invalid. It showed no
compile-time change and a 25% slower tick. The new pass manager skips `optnone`
functions only through `llvm::OptNoneInstrumentation`. `optimize_project_module()`
did not register it, so the roots were still optimized; only inlining into them was
refused. The instrumentation is now always registered (2026-11-07, awaiting
measurement).

Pass profile at 64 voices (2026-11-07, `IV_GRAPH_JIT_PROFILE_DIR`, one cutover
compilation):
- *Input to O3.* 34,338 instructions in 425 functions. The generated
  `__iv_graph_root_tick_block` accounts for 16,583 instructions and the other
  generated `__iv_graph_*` functions for 7,237. Package code compiled at Clang O0
  accounts for 10,514 instructions in about 385 functions.
- *Output of O3.* 97,416 instructions in 40 functions. The root alone is 91,165,
  5.5 times its input size. Package code shrinks to 2,130.
- *Optimization (3.47 s).* Loop and scalar passes dominate: `LoopUnrollPass` and
  `LoopRotatePass` 0.41 s each, `GVNPass` 0.40 s, `InstCombinePass` 0.39 s, and
  `IndVarSimplifyPass` 0.18 s.
- *Code generation (2.91 s).* X86 DAG instruction selection 0.63 s and greedy
  register allocation 0.62 s, both proportional to the optimized root.

The hypothesis that the per-type wrappers grow with the lane count is refuted. The
four `__iv_graph_primitive_*_tick_block_batch` wrappers end at 469-609 instructions.
The cost follows the root's growth during optimization.

Profiling knobs (2026-11-07):
- `IV_GRAPH_JIT_NO_UNROLL=1` disabled partial, runtime, and full loop unrolling in
  the project pipeline.
- `IV_GRAPH_JIT_PRELINK_PACKAGES=1` ran LLVM's ThinLTO pre-link simplification
  pipeline (O2) inside `GraphJit`. It ran once per package revision, the first time
  a compilation used that revision, and later compilations lowered from the
  simplified bitcode. At the time the design was O0 package bitcode with
  whole-project optimization; the knob did not change that design.
- `IV_GRAPH_JIT_DUMP_IR=1`, together with `IV_GRAPH_JIT_PROFILE_DIR`, writes the
  module before and after optimization as `<stem>-before.ll` and `<stem>-after.ll`.

Measured at 64 voices (2026-11-07, repository owner, `iv_edit_latency_benchmark`,
per-edit medians; the tick is the realtime tick mean of a 5,333 µs block):

| Knobs | Edit wall | Optimize | Code generation | Tick |
|---|---|---|---|---|
| none | 6.6 s | 3.6 s | 2.9 s | 1,535 µs |
| `NO_UNROLL` | 4.9 s | 2.5 s | 2.3 s | 1,540 µs |
| `PRELINK_PACKAGES` | 4.75 s | 1.95 s | 2.75 s | 1,500 µs |
| both | 3.3 s | 1.13 s | 2.13 s | 1,520 µs |
| `ROOT_OPTNONE` (fixed) | 1.81 s | 0.34 s | 1.41 s | 1,950 µs |

With `PRELINK_PACKAGES`, the initial compilation's package parse took 2.0 s. That is
the one-time pre-link of the whole built-in package.

Decisions taken from these measurements:
- *Pre-link at package build (accepted change, 2026-11-07).* Package translation
  units now compile to optimizer-ready Clang LLVM (`-O2 -Xclang
  -disable-llvm-passes -flto=full`), and so do the DSL PCH and the clangd command.
  `iv_package_finalize` removes Clang's O0 barrier pair (`optnone` with `noinline`)
  from any O0 LLVM input. It then runs the ThinLTO pre-link pipeline (O2) once over
  the linked package. (The pre-link first ran after `preserve_package_code()` had
  pinned every reachable function in `llvm.used`; that pin is removed, see "Friction:
  pinned copies left gaps in the linked `.eh_frame`" below.)
  `optimize_project_module()` no longer removes barriers, and the in-process knob is
  gone. The superseded O0 contract is in
  [Package Bitcode O0 Contract](historical/package_bitcode_o0_contract_2026-11-07.md).
  This is awaiting measurement: the expectation is the `PRELINK_PACKAGES` numbers,
  plus whatever TBAA and lifetime markers add.
- *No loop unrolling per edit (implementation decision, 2026-11-07).* The project
  pipeline runs with `PipelineTuningOptions::LoopUnrolling` off. It saved about
  1.6 s per edit, and no tick change was measurable. Loops whose trip counts are
  package constants are fully unrolled by the package pre-link pipeline.
  `IV_GRAPH_JIT_UNROLL=1` re-enables unrolling for comparison. A node whose loops
  only become constant after realization may want unrolling back. Measure such a
  node before deciding how to handle it.

Verified from the 4-voice IR dump:
- *No TBAA, no lifetime markers.* The O0 package bitcode contained no `!tbaa`
  metadata and no `llvm.lifetime` markers.
- *The root's growth is per-instance inlining of generated plumbing.* The
  unoptimized `__iv_graph_root_tick_block` (1,043 instructions, 49 blocks) makes 16
  per-instance calls to `__iv_graph_primitive_*_tick_block` and two batched calls.
  For each instance it copies a binding template (216 or 464 bytes) into an alloca
  with `memcpy` and patches the storage pointers in place. After O3, the root has
  5,635 instructions in 579 blocks: 160 `memcpy` calls, 86 `assertion_failed` calls,
  10 `__cxa_throw` calls, and no floating-point or vector instructions. The checks
  come from `build_request.h`, for example `ctx.continuous_input_bindings.size() ==
  input_count`. They validate a `ReflectedNodeTickContext` that the root builds at
  runtime, block by block, from values the lowerer already knows. They do not fold
  away, because the context escapes into calls that stay out of line, such as the
  `tick_node_block<Constant>` lambda. The DSP itself stays in out-of-line package
  functions (`SawOscillator::tick`, `SimpleIirLowPass::tick`).
- *Package code is compiled for baseline x86-64.* Every package function carries
  `"target-cpu"="x86-64"` with SSE2 features. Out-of-line package functions are
  therefore generated without the host features that `GraphJitKernelSpecialization`
  records for the project target. This is a realtime defect; it does not affect edit
  latency, and it is not yet addressed.

Measured after pre-link at package build and no per-edit unrolling (2026-11-07, 64
voices, repository owner; no tests were run on this state. An earlier version of
this line said all 790 tests passed, which no test run supported):
- *New defaults.* 3.3-3.5 s per edit (optimize 1.12 s, code generation 2.1-2.3 s).
  The tick fell to 1,265-1,300 µs from about 1,520 µs with in-process pre-link of O0
  IR. The 4-voice dump now has 1,168 `!tbaa` references and 144 lifetime markers in
  the project module; the O0 bitcode had none.
- *`IV_GRAPH_JIT_UNROLL=1` on top.* 5.5-5.9 s per edit with an unchanged tick
  (1,260-1,276 µs). This confirms that unrolling does not pay for itself per edit.
- *`iv_package_finalize`.* `package_prelink_optimize_us` is 2.49 s of 2.71 s for the
  built-in package. This is package-build time.
- *`IV_GRAPH_JIT_NOINLINE_PRIMITIVES=1`: invalid run.* The package callback adapters
  in `build_request.h` and `tick.h` are `IV_FORCEINLINE` (`always_inline`). Adding
  `noinline` to them produced invalid IR. The always-inliner still inlined them, so
  optimization was unchanged at 1.1 s. The verifier then rejected the optimized
  module, so nothing was materialized or accepted, and the measured tick (about 1 µs)
  was an empty executor. The knob now removes `alwaysinline` first. The benchmark
  summary now prints how many generations per edit kind were accepted.

The package functions' attribute group also showed the NixOS clang wrapper's
hardening flags on realtime code. The 4-voice module had `sspstrong` on 43
functions, `"zero-call-used-regs"="used-gpr"` and `"frame-pointer"="all"` on 10, and
`"probe-stack"="inline-asm"` on 5. The switch to `-O2` also activated
`_FORTIFY_SOURCE`, which put 7 `__memcpy_chk` calls into the primitive operations; O3
folded them here. `-fwrapv`, `_GLIBCXX_ASSERTIONS`, pattern initialization, and CET
were not active in this build environment.

*No hardening in package code (accepted change, human co-author, 2026-11-07).*
Package code runs in the realtime callback, so it is compiled without hardening,
whatever the build environment enables. `module/template/PackageCompileFlags.cmake`
defines `IV_PACKAGE_COMPILE_FLAGS` once. Package translation units, the DSL PCH, and
the clangd command all use it. The wrapper adds its hardening and frame-pointer
flags before ours, so ours override them: the stack protector, stack-clash probes,
register zeroing, CET, pattern initialization, `-fwrapv`, `-fno-strict-aliasing`
(which would remove TBAA), `_GLIBCXX_ASSERTIONS`, and frame pointers. The wrapper
defines `_FORTIFY_SOURCE` after our flags, so `module_pch.h` undefines it before any
system header. Host code keeps the environment's flags. Package code under `perf`
no longer has frame pointers, so call graphs through it need DWARF unwinding. The
baseline `"target-cpu"="x86-64"` remains a separate open realtime defect.

The root's per-instance plumbing is the remaining structural cost, and it scales
with instance count. The ROOT_OPTNONE row above shows its two sides. Optimizing it
cost about 3 s per edit at 64 voices, and generating code for it cost 1.4 s even
unoptimized. Without root optimization, the tick was 27% slower.

Measured without package hardening (2026-11-07, 64 voices, repository owner; all
generations accepted):

| Knobs | Edit wall | Optimize | Code generation | Tick |
|---|---|---|---|---|
| none | 3.1 s | 1.08 s | 1.97 s | 1,166-1,199 µs |
| `NOINLINE_PRIMITIVES` | 0.61 s | 0.25 s | 0.33 s | 1,112-1,195 µs |
| `NOINLINE_PRIMITIVES`, `CODEGEN_LEVEL=1` | 0.61 s | 0.25 s | 0.33 s | 1,150-1,163 µs |
| `ROOT_OPTNONE` | 1.0 s | 0.15 s | 0.81 s | 1,215-1,251 µs |

Dropping hardening lowered the tick from 1,265-1,300 µs to about 1,170 µs. The 4-voice
dump confirms that the hardening attributes and `__memcpy_chk` calls are gone, and
TBAA is intact.

*Primitive operations stay out of line (implementation decision, 2026-11-07).*
`optimize_project_module()` removes `alwaysinline` (from `IV_FORCEINLINE`) from
every imported `__iv_graph_primitive_*` function and marks it `noinline`. Inlining
them into the root cost about 2.5 s per edit at 64 voices and bought no measurable
tick. The root builds each instance's `ReflectedNodeTickContext` at runtime, so
inlining had nothing constant to specialize. `IV_GRAPH_JIT_INLINE_PRIMITIVES=1`
restores inlining for comparison. Whole-project inlining remains possible, as
`graph_jit_direction.md` requires; it is no longer the default.

Observations that follow from this:
- *Primitive code is edit-invariant.* `import_primitive_callback()` only renames a
  package callback and promotes its linkage, so every `__iv_graph_primitive_*`
  function and its imported closure depend only on the package revision and the
  callback's source symbol. Under the edit-invariance principle, that code should
  be compiled before any compilation is launched, not on every edit.
- *Codegen level does not matter* once the primitives are out of line.
- *An unoptimized root is not cheaper.* `ROOT_OPTNONE` cost more code generation
  (0.81 s) than optimizing the root and generating code for it (about 0.58 s
  together).

*Primitive code compiled once per package revision (accepted change, human
co-author, 2026-11-07, "for now").* The normative statement is in
`graph_jit_direction.md`, in the section on the two JITs:
- The package build compiles node code once, for the host target, and the accepted
  `PackageRevision` carries it. The first version of this statement named `GraphJit`
  as the compiler; the module is corrected below.
- A per-edit compilation produces only the root, or root data, plus content-cached
  fused cross-type code where the cost model chooses fusion.
- Whole-project inlining is one of the cost model's choices, not the default.

Compiling node code for the host target also resolves the baseline
`"target-cpu"="x86-64"` defect for it.

*Friction: package code delivered to `GraphJit` (2026-11-07, resolved).* The first
implementation compiled package code in `GraphJit`, on a new
`iv_runtime_package_jit_batch_completed_event` that `PackageWatcher` raised before
publishing revisions. One package refresh then entered `GraphJit` twice: once on that
event, and once through `PackageDefinitions` → `NodeDefinitions` → `ProjectGraph`.
`LinkerEventModuleFrameScope` threw, the `PackageWatcherService` worker exited, and
no definitions loaded, so the edit benchmark hung for 30 minutes on every shape. The
tests missed it, because the GraphJit fixture called the handler directly. The root
cause was ownership: `PackageJit` returns self-contained revisions, and `GraphJit` was
keeping per-revision state that duplicated that lifetime. The human co-author approved
the replacement below. The first implementation and its status text are in
[GraphJit Package Code Delivery](historical/graph_jit_package_code_delivery_2026-11-07.md).

*Node code compiled once per package build (implementation status, 2026-11-07;
built, all 790 tests pass, measured below; repository owner).*
- **Finalizer.** After it writes the finalized LLVM and the node-event protocol,
  `iv_package_finalize` runs these steps, each timed as its own stage:
  - `reduce_to_node_code()` reduces the same module in place:
    - It builds `iv.node_code_records`: one `NodeCodeRecordData` per
      `iv_node_types` record, sorted by `NodeCodeKey`, holding the record's
      operations with `declare_node` cleared.
    - It builds `iv.node_code_retained_globals`, one `NodeCodeRetainedGlobalData`
      per retained-global index. It finds the retained-global table through the
      `iv_package_retained_globals` accessor, so a renamed table is still found. An
      entry that is not a constant, initialized, non-thread-local global gets a null
      address.
    - It erases every appending global: the `used` lists, the static constructors
      and destructors, and the annotations. Node runtime operations reference no
      mutable package global, so node code needs no constructors.
    - It internalizes everything except the two table accessors. It sets each
      function's `target-cpu` to the host, and its `target-features` to the host
      features followed by the function's own.
  - `optimize_node_code()` runs GlobalDCE, then O3 with Clang's O3 tuning.
  - `write_node_code_object()` emits `<output>.node-code.o`. Code generation uses
    `Reloc::PIC_`, the small code model, and `CodeGenOptLevel::Aggressive`.

  The stages are `node_code_reduce`, `node_code_optimize`, and `node_code_codegen`.
  `ModuleSupport.cmake` lists the object as a byproduct.
- **ABI.** `module/abi.h` defines `NodeCodeRecordData` and
  `NodeCodeRetainedGlobalData`, with accessor types `iv_package_node_code_records_fn`
  and `iv_package_node_code_retained_globals_fn`. They raised `IV_PACKAGE_ABI_VERSION`
  to 5; removing `ContinuousPortStorageBinding::storage_latency` later raised it to 6
  (see the latent-output friction record below).
- **Artifact signature.** `ModuleLoader` adds `host-cpu=`: the host CPU name and its
  sorted features. A cached object therefore never loads on a CPU it was not compiled
  for, for example a prebuilt built-in or common package artifact used on another
  machine.
- **Loading.** `ModuleLoader` links the object into its own `JITDylib`
  (`LoadedPackageCode::node_code_dylib`) beside the package's, with the same
  generators, and removes it with the package. It copies the two tables into
  `LoadedPackageCode`, checks that the records are strictly sorted and that the
  retained-global count matches the package's, and returns them through
  `ModuleLoader::LoadedPackage`. `PackageJit` moves them into
  `PackageRevision::node_code_records` and `node_code_retained_globals`.
- **Separate `JITDylib`.** The node-code object links in its own `JITDylib`, so its
  linker-private weak definitions, such as `DW.ref.__gxx_personality_v0`, never
  resolve to the configuration graph's. The first version used the package's
  `JITDylib`. That sharing was suspected of causing the eight test aborts below,
  and was ruled out: the aborts persisted with the separate `JITDylib`.
- **Friction: pinned copies left gaps in the linked `.eh_frame` (2026-11-07,
  resolved; all 790 tests pass).**
  - *Symptom.* Eight tests aborted. In each, an exception thrown through
    JIT-compiled package configuration code reached `std::terminate`. These are
    `ModuleLoaderFailures` tests 10, 11, 16 and 20; `ModuleLoaderPackages` 12 and
    14; `PackagePipeline` 695; and `IvModuleSourceIntrospection` 707.
  - *Mechanism.* The repository owner allowed gdb for this investigation. Core dumps
    and a gdb session showed four steps:
    1. `preserve_package_code()` pinned every function reachable from the package
       entry points in `llvm.used`.
    2. The new O2 pre-link inlined most of those functions but had to keep the
       pinned out-of-line copies. In the failing package, 813 of 979 pinned
       functions had no remaining reference.
    3. JITLink dead-strips such unreferenced local functions and their `.eh_frame`
       records. It splits `.eh_frame` into one block per record, and each block
       keeps its original alignment of 8 with offset 0 or 4. A pruned record of
       size ≡ 4 mod 8 therefore shifts every following record by 4 bytes, and
       JITLink fills the shift with zero padding.
    4. libgcc reads a zero length as the end of the section. It never saw the FDEs
       after the first gap, including those of the configuration functions, so
       unwinding stopped there.

    The FDE, its CFI, and its LSDA for the failing frame were all present and
    correct. JITLink's own terminator block sorts last.
  - *Why it was new.* At O0, no pinned function was inlined away, so nothing was
    pruned.
  - *Fix.* The pin existed so that `GraphJit` could import package callback bodies;
    node code removed that need. `remove_unreachable_package_code()` replaces
    `preserve_package_code()`. It checks the entry points, adds nothing to
    `llvm.used`, and runs GlobalDCE. `prelink_optimize_package()` now ends with
    GlobalDCE.
  - *Still latent.* JITLink's record-alignment behaviour still breaks `.eh_frame`
    whenever it prunes a record of size ≡ 4 mod 8 from any in-process graph. After
    this fix, the package, node-code, and project graphs contain no unreferenced
    local functions, so nothing is pruned. A future change that leaves dead local
    code in a JIT-linked object would bring the failure back. This is an LLVM
    JITLink defect to report upstream.
- **Compilation.** `compile_impl()` finds each node's record by binary search
  (`node_code_record()`). `verify_compiler_record()` now compares the presence of
  each optional callback with the `NodeCompilerRecord`. The state ABI comes from the
  `NodeCompilerRecord` itself. `ImportedNodeCode` names each imported address
  `__iv_node_code_<hex>` or `__iv_node_data_<hex>`. `materialize_project_module()`
  defines those names as absolute symbols in the generation `JITDylib`, and
  `ProjectCodeLifetime::package_revisions` pins the revisions that own the code.
  `GraphJitCompileTimings::package_code_us` is gone, because there is nothing left to
  time.
- **Removed.** `iv_runtime_package_jit_batch_completed_event`,
  `package_watcher_graph_jit_bridge`, `GraphJit::handle_package_jit_batch_completed()`,
  `GraphJit::Impl::package_code_`, the package `TargetMachine`, the project `LLJIT`'s
  concurrent compilation, and the test fixture's `deliver_package_code()`.
- **Restored callback ABI check (2026-11-10).** The project module calls node code by
  address, so GraphJit cannot inspect a callback's LLVM function type at import time.
  `iv_package_finalize` now checks every non-null runtime callback in each
  `NodeCompilerRecord` before it emits `.node-code.o`. Finalization and GraphJit use
  the same `graph_jit::package_callback_type()` implementation, so a callback cannot
  reach the address-only import path with a different LLVM function type.
  `PackageCallbackAbi.AcceptsOnlyTheDeclaredFunctionTypeAndCallingConvention` checks
  every callback ABI against both a wrong function type and a wrong calling convention.
- **Measured (2026-11-07, repository owner, size 64, medians over five edits of
  each kind).** Edit wall is from the edit to the executor accepting the
  generation. Optimize and code generation are the transition compilation's.

  | Shape | Instances | Edit wall | Optimize | Code generation | Tick mean |
  |---|---|---|---|---|---|
  | `voices` | 320 | 385-393 ms | 148-149 ms | 223-228 ms | 954-968 µs |
  | `chain` | 68 | 87-89 ms | 33 ms | 50-51 ms | 638-649 µs |
  | `mixed` | 448 | 609-619 ms | 223-231 ms | 362-372 ms | 1,548-1,571 µs |

  For `voices`, the edit fell from 0.61 s and the tick from 1,112-1,195 µs, the
  last measurement with primitive operations out of line but compiled per edit.
  That run was not yet node code. The tick gain is node code compiled at O3 for the
  host CPU. Planning, emission, and verification together take about 12 ms at 320
  instances. Optimizing and generating code for the root is 96% of each
  compilation, and it grows with instance count. That makes it the target of root
  as data (item 3). `chain` already meets the 0.2 s budget. The first `feedback`
  run was invalid (see the shape list above).

  The full test suite (790 tests, `ctest -j16`) fell from about 5 minutes before
  Front 16 to under 1 minute (repository owner, 2026-11-07). That makes adding
  tests much cheaper.

  With the module-based shape, `feedback` (64 voices, 513 bundles) compiles and
  runs its first generation: 0.60 s, optimize 215 ms, code generation 363 ms, tick
  mean 1,978 µs. The first edit after that fails. `GraphExecutor` rejects the
  cutover with "sample transition source cannot represent its semantic interval"
  from `validate_sample_interval()` in
  `graph_jit/realtime_port_state_transition.cpp`. The predecessor's compact-carry
  storage does not cover the interval the transition inherits. The message now
  reports the interval and the storage geometry. It read "[0, 1) in
  compact-carry storage of 1 frames (0 carried future, timeline offset 0)".

  *Friction: latent Tick output state across a cutover (earlier-front oversight,
  found 2026-11-07; decided and implemented 2026-11-07; verification pending).*
  - *The state.* The failing state is `SawOscillator`'s `out`, which has authored
    latency 1 (`tick_output_latency`). The `detach()` loop only makes this graph
    pick compact carry for it.
  - *Physical layout (verified in source and by tests).* A Tick output writes each
    value at its own tick index: lowering set
    `ContinuousPortStorageBinding::storage_latency` to 0 (the field is now removed)
    and gives the output the callback's `sequential_begin_index`. `OutputPort::update()` revises the last
    `latency` values, and consumers read `latency` frames back through their
    `read_latency`. At a block boundary, the output's owned latency window is
    therefore the last `latency` frames before the boundary. The producer carry's
    `future_frames = 0` is physically correct.
  - *First diagnosis, wrong and reverted.* Setting the producer carry's
    `future_frames` to the output latency made four runtime tests read
    unrestored frames: `TickOnlySampleNodePreservesContextAcrossPrimitiveSlices`,
    `SampleLatencyCompensation`, and two `UnequalLatencySampleFeedbackMigrates*`
    tests. The carry sites now state `future_frames = 0` explicitly, with the
    reason.
  - *Actual mismatch.* `make_transition()` places the latency window at semantic
    `[0, latency)` and history at `[-history, 0)`, which is the consumers'
    latency-compensated timeline. The transfer maps semantic `r` to physical
    `cursor + r + timeline_offset`. `plan_sample_realtime_port_state_realizations()`
    gives retained output views offset 0, so the latency window maps past the
    cutover, onto frames the producer has not written. Compact carry rejects
    this (the observed error). A ring would accept it, copy stale values, and
    leave the real window unmigrated. The mapping that matches the physical
    layout is a retained offset of `-latency`.
  - *Two conventions.* sequential_port_storage_planning.md states that
    "Tick-output authored storage latency is positive". That is the *finalization
    index* convention: storage files a value at `index + latency`, and the producer
    writes ahead through `storage_latency`. The implementation uses the *authoring
    index* convention: storage files a value at its own index, and consumers
    include the latency in `read_latency`. `make_transition()`, the realization
    offsets, and the disconnected-output carry (`SampleSinkStorageRequest` sets a
    physical `future_frames = latency`) were written for the documented convention.
    That carry commits values past the root invocation's end, and
    `DisconnectedSampleOutputKeepsDeclaredHistory` encodes it.
  - *Destination analysis (2026-11-07).* Both conventions implement latency
    compensation: each continuous value has one `GlobalIndex` position, and each
    reader may observe it only once its author can no longer revise it, aligned
    with every other path into that reader. They differ in which index storage
    uses.
    - With the authoring index, the producer writes at its own index, and persisted
      capture records each value at its own index. Each realtime consumer applies
      one delay, `read_latency`, which it already needs for path equalization.
    - Every owned window then lies before the next tick: the last
      `history + latency` values, of which latency only marks the revisable ones.
      Carries need only `retained_frames`, transitions need only unsigned extents,
      and `storage_latency` (always 0) becomes removable, which saves an add on every
      output access.
    - The finalization index adds an offset on the producer's write and on
      persisted capture. It also splits each output's owned window around the next
      tick (history before, latency after), which is what requires signed
      intervals or `future_frames`.
    - Recommendation: standardize on the authoring index and revise the design text.
      That means retained output windows at `[-(history + latency), 0)`, the
      disconnected-output carry at `future_frames = 0`, and removal of
      `storage_latency`. A runtime test that cuts over a latent output and checks
      sample continuity, in both ring and compact-carry storage, is needed. The
      repository owner chose the authoring index on 2026-11-07.
  - *Implementation (2026-11-07; build, tests, and the `feedback` benchmark rerun
    are pending).*
    - Design text: sequential_port_storage_planning.md,
      dsp_execution_storage_glossary.md, and graph_jit_direction.md now state that a
      Tick sample output owns its last `history + latency` values before its next
      tick. The superseded text is in
      [historical/tick_output_finalization_index_2026-11-07.md](historical/tick_output_finalization_index_2026-11-07.md).
    - `inventory_realtime_port_states()` (connection_plan.cpp) emits one
      `tick_output_history` record per sample-output channel, with extent
      `history + latency`. `tick_output_latency` is now an event-only role
      (realtime_port_state_plan.h).
    - `plan_sample_realtime_port_state_realizations()` (lowering_plan.cpp) requires
      that record and gives the authored and retained output views offset 0.
      `specialize_transition_plan()` (lowering.cpp) accepts only
      `tick_output_history` for a sample output window.
    - sample_storage_plan.cpp: the disconnected-output carry uses
      `future_frames = 0`, and every carry states its `future_frames` explicitly.
      `SampleCarryOperationPlan::future_frames` documents that only shifted composed
      feedback carries future frames.
    - `ContinuousPortStorageBinding::storage_latency` is removed from
      continuous_port.h and from its users (activity_runtime.cpp,
      tick_invocation_frame.cpp, lowering.cpp, and four test helpers). Compiled node
      code reads that record. `IV_PACKAGE_ABI_VERSION` was raised to 6 for this; the
      human co-author reset every version constant to 0 on 2026-11-08 (see the
      authored `State` migration record below).
    - Consequence: a disconnected latent output's state is now
      `transfer_to_steady_storage`. Its compact carry has one semantic user instead of
      two, so the cutover writes the window into the steady successor carry and
      needs no transition root. `TransitionRootPreservesSampleOutputWindow` is now
      `CutoverTransfersSampleOutputWindowIntoSteadyCarry`.
    - New runtime tests: `LatentSampleOutputWindowSurvivesCutoverInCompactCarry`,
      `LatentSampleOutputWindowSurvivesCutoverInRing`,
      `LatentSampleConsumerStaysFinalAcrossCutoverInCompactCarry`, and
      `LatentSampleConsumerStaysFinalAcrossCutoverInRing`. Their source keeps the
      newest values of each block provisional and revises them through
      `OutputPort::update()` on its next tick. Each block checks every value of the
      source's window, and a consumer's latency-compensated reads, before and after
      the cutover.
    - Updated expectations: `DisconnectedSampleOutputKeepsDeclaredHistory`,
      `InventoriesStableNodeOwnedRealtimePortState`,
      `KeepsAnonymousRealtimePortStateGenerationLocal`, and
      `MatchesStableStateAcrossGenerationLocalPorts`.
    - Renamed under the terminology rule: `sequential_begin_index`,
      `contiguous_begin_index`, `begin_index_bits`, `lattice_index_bits`,
      `emit_sequential_begin_index`, `store_sequential_begin_index`, and
      `validate_sequential_bindings`.
    - Unresolved, not changed: `ContinuousOutputPort::get()` asserts
      `offset <= history + latency`. At `cursor_ == 0`, `offset == history + latency`
      reads one value older than the owned window, which a compact carry does not
      restore. No document states the `get()` bound.
  - *First run (repository owner, 2026-11-07): 792 of 794 tests passed.* Both
    `LatentSampleConsumerStaysFinalAcrossCutover*` tests failed, only in the
    consumer's checks and only while the transition root ran. The source's window
    checks passed in every block.
    - *Records.* When an output window and a consumer's input history share one
      steady producer representation, `specialize_transition_plan()` (lowering.cpp)
      gives each its own transition-only ring. It adds two `SampleMaterializationPlan`
      mirrors at the producer's step: output ring → steady representation with
      `retained_before = latency`, then steady representation → consumer ring with
      `retained_before = latest_read_latency = 0`.
      `emit_sample_materialization()` copies
      `[index - retained_before, index + block - latest_read_latency)`.
    - *Defect.* The consumer mirror copied `[index, index + block)`, the values just
      authored. The consumer reads `[index - delay - history, index - delay + block)`,
      where delay is its binding's `read_latency + frame_delay`. With latency 2, the
      first transition block never received the producer's two inherited, revised
      values (2 mismatches). With latency 1000, every value the producer authors in
      a block is still provisional when copied, its later `OutputPort::update()`
      never reached the consumer's ring, and every consumer read was wrong for the
      whole transition (64, then 68 mismatches per block).
    - *Fix.* The consumer mirror now uses
      `retained_before = latest_read_latency = delay`, so it copies exactly the
      values the consumer newly reads, after the output mirror has propagated the
      producer's revisions. Channels that share one consumer ring must share the
      delay; otherwise lowering reports an error.
    - *Open friction, not changed.* A consumer whose delay exceeds its producer's
      latency (feed-forward path-latency equalization, "compiler-owned read
      compensation" in graph_jit_direction.md) also reads, in its first transition
      blocks, positions older than the output mirror's `[index - latency, ...)`
      range. Inside the producer's history those values exist only in the output
      ring; beyond it, no semantic-state record owns them. No document says whether
      compensation delay survives a cutover. A decision is needed.
    - *Reproduction tests (2026-11-08).*
      `CompensatedInputUnaffectedBySecondReaderMove`,
      `CompensatedInputUnaffectedByHistoryReaderMove`, and
      `CompensatedHistoryInputUnaffectedByHistoryReaderMove` (graph_jit_gtest.cpp)
      edit a graph without touching a compensated probe's connections. In the
      collaborator's run each fails as predicted, with 7 mismatches in its first block
      after the cutover.
    - *Decision (human co-author, 2026-11-08).* The in-flight values belong to the
      reading input, as positions `[P, P + c)` of one window `[-history, c)`, where `c`
      is its compensation. A rewired input plays its in-flight values before the new
      source's values. Recorded in sequential_port_storage_planning.md, section "Port
      windows are semantically node-owned even when storage is shared"; the replaced
      text is in historical/input_owned_history_only_2026-11-08.md.
    - *More reproduction tests (2026-11-08; not yet run), each expected to fail with 7
      wrong values in its first block after the cutover:*
      `CompensatedHistoryInputOfLatentSourceUnaffectedByHistoryReaderMove` (a source
      with history and latency of its own, through the transition root),
      `CompensatedInputUnaffectedByHistoryReaderLeavingItsSource` (producer storage
      shrinking from a ring to a compact carry),
      `ConvertedCompensatedInputUnaffectedByHistoryReaderMove` (a mono to interleaved
      conversion), `GrowingCompensationAddsFreshValuesAfterInFlightValues`,
      `ShrinkingCompensationDropsTheNewestInFlightValues`, and
      `RewiredCompensatedInputReadsItsInFlightValuesFirst`.
    - *Collaborator run (2026-11-08):* those six fail as predicted, each with 7 wrong
      values in its first block after the cutover.
    - *Remaining reproduction tests (2026-11-08; not yet run).* None needed a
      production change:
      - `ComposedCompensatedChannelUnaffectedByHistoryReaderMove` and
        `InterleavedComposedCompensatedChannelUnaffectedByHistoryReaderMove`: a
        stereo input composed from the ramp and the delay node. The planar input's
        channels alias their sources with a per-channel `frame_delay`
        (sample_storage_plan.cpp). New probes compare the two channels. Expected to
        fail with 7 mismatches.
      - `FeedbackLoopUnaffectedByHistoryReaderMove`: a counter fed back through a
        `detach()` of latency 5. Its nodes come from a package module, because the
        existing feedback graphs are built in test code through
        `configure_package_definition_provider()` and have anonymous nodes. Moving
        the history reader onto the counter's output gives that producer retained
        frames, so the feedback edge no longer uses its separate home ring. Expected
        to fail at every position after the cutover, because the counter restarts
        from its lost in-flight values.
      - `LongCompensationInFlightValuesOutlastTheTransitionRoot`: a compensation of
        998, whose in-flight values outlast a transition root that expires after a
        few values. Expected to fail at most of the 998 in-flight positions.
      - `CompensatedInputUnaffectedByAddedLeadingNode` and
        `GrowingSourceLatencyAddsFreshValuesAfterInFlightValues` build one module
        twice with different construction arguments, through the new test helper
        `configured_module_graph_with_arguments()`. It repeats the loader's calls
        (module/loader.cpp) with the arguments passed to `module_build`. Each test
        first checks that every surviving node keeps its stable identity. Expected
        to fail with 7 wrong values.
    - *Collaborator run (2026-11-08): 795 of 810 tests pass.* The composed, feedback,
      and long-compensation tests fail as predicted. The two argument-built tests
      failed in their fixture: configuration type identities exist only in package
      code, so `configuration_argument()` in host test code carried
      `<unavailable type identity>`. `configured_module_graph_with_arguments()` now
      gives each argument its parameter's identity from the module's
      `RegisteredSignature`.
    - *Implementation plan (2026-11-08; approved by the co-author).*
      - Each Sequential input's record covers `[-history, c)`, where `c` is its read
        delay beyond its source's latency, including a `detach()` delay. Cold
        metadata only.
      - Every record maps its inherited window into the successor's steady storage
        at the successor's index mapping, as `transfer_to_steady_storage` already
        does for direct views. Materialized and composed views transfer through
        their per-source views instead of being classified `transition_only_storage`.
      - Records whose transfers agree on every shared target slot (the same
        predecessor slot, or both fresh) share the steady target. This replaces the
        blanket shared-target downgrade. Only records that disagree move to
        transition-only storage, choosing the provenance class with the smaller total
        extent.
      - A moved record's transition-only ring holds `[-history, c)`; its mirror copies
        from the source's finalization frontier, with an 8-byte resume index only
        where the record's source latency grew.
      - Cost: no steady memory or time. The cutover copies `history + c` values per
        record between audio callbacks. Transition-only memory exists only for moved
        records.
    - *Implemented (2026-11-08; collaborator build and test run pending).*
      - `RealtimePortStateRequirement::in_flight_samples`, set per input channel by
        `inventory_sequential_input_in_flight_values()` (connection_plan.cpp) right
        after `plan_sample_latency_compensation()`. Every channel of an input fed
        only through `tick_to_sequential` storage now has a record, even an empty
        one, so a read delay that grows from nothing still finds its window.
      - `SampleRealtimePortStateStorageView::producer` and `producer_channel`, set
        in `plan_sample_realtime_port_state_realizations()` (lowering_plan.cpp)
        for a producer's canonical representation.
      - `RealtimePortStateTransition::target_begin`/`target_end`.
        `apply_input_window()` gives an input `[-history', T)` from
        `input_resume_position()`.
      - Converted (`materialized`) and composed inputs whose sources pair by
        producer, channel, and read delay transfer per source
        (`sample_sources_pair_by_producer()`).
      - A window that inherits nothing gets a fresh-only transfer, which zero-fills
        its target and reads no predecessor storage.
      - `resolve_sample_transfer_conflicts()` replaces the sample half of the
        shared-target downgrade in `classify_shared_successor_storage()`.
      - Consumer suffix mirrors in `specialize_transition_plan()` (lowering.cpp)
        copy at `d - T`. The executor's `*_ordinal` locals are renamed `*_index`.
    - *First collaborator run (2026-11-08): 758 of 810 tests pass.*
      - 50 failures had one cause: `plan_sample_realtime_port_state_realizations()`
        and `index_realizations()` rejected a sample record with no history. Both
        now accept one for a Sequential input.
      - `NewStateSharingSampleTargetRequiresTransitionStorage` showed a missing rule:
        a state without a predecessor, or an anonymous one, starts fresh, so its
        window must not show values another state inherited.
        `resolve_sample_transfer_conflicts()` now treats every such non-empty window
        as an all-fresh claim and moves any kept transfer that puts inherited
        values inside it to transition-only storage.
      - `TransitionRootReconstructsMaterializedSampleHistory` expected a transition
        root that only the old shared-target rule produced. Its converted history
        now transfers through its source, so the test is renamed
        `CutoverTransfersMaterializedSampleHistoryThroughItsSource` and checks a
        steady cutover against the same reference.
    - *Second collaborator run (2026-11-08): 804 of 810 tests pass.*
      - The six failures are the projected and unequal-latency `detach()` feedback
        tests, which compile one generation. Each failed with "GraphJit composed
        port state has an ambiguous target channel". Their converted node's
        stereo input reads the feedback loop through a routed connection. Before
        `inventory_sequential_input_in_flight_values()` existed, it had no record,
        because its history is 0.
      - Its storage is the final `SampleCompositionPlan` built for routed feedback
        (sample_storage_plan.cpp). That plan has one contribution with a mono
        `source_layout`, one source per feedback timeline, and an explicit
        `SampleChannelTransformPlan`. The composed branch of
        `plan_sample_realtime_port_state_realizations()` took its source count
        from `channel_count(source_layout)` and its coefficients from
        `channel_conversion_coefficient()`. It now reads both from the transform
        when the transform is explicit, as `emit_sample_composition()` does
        (lowering.cpp).
      - *Reproduction test, not yet run:*
        `RoutedSampleDetachFeedbackUnaffectedByRecompile`. The package module
        `routed_feedback_module` builds that loop with `detach()` latencies 6 and 7
        on the two channels. The existing feedback graphs are built in test code
        and have anonymous nodes. The test activates an unchanged recompile at
        index 64 and compares the converted node's reads with an uninterrupted
        run. Expected to fail. A routed feedback timeline is not a canonical
        producer representation, so its view has no `producer`, and
        `sample_sources_pair_by_producer()` fails for it. The input's record
        therefore moves to transition-only storage at every cutover. Its composition
        runs before the consumer (`before_execution_position`).
        `specialize_transition_plan()` nevertheless appends the mirror to that
        consumer step's `sample_transition_materializations_after`, so the consumer
        reads its suffix before the mirror writes the block.
    - *Third collaborator run (2026-11-08): 810 of 811 tests pass.* Only
      `RoutedSampleDetachFeedbackUnaffectedByRecompile` fails, as predicted. The
      reads at 64 and 68 are right, because they fall inside the inherited windows
      of 6 and 7 values. Every recorded read from 71 through 95 is 0.
    - *Fix (2026-11-08; not yet run).*
      - `emit_sample_composition_write()` (lowering.cpp) writes a routed feedback
        timeline at source frame + read latency. It covers only source frames from
        `revision_frames` (the source latency) before the block start. So in the
        successor's first block it writes timeline indices from
        `P + read_latency - source_latency`. The input's window `[0, c)` maps to
        timeline indices `[P - detach, P + read_latency - source_latency)`. The
        writer therefore never rewrites an inherited position.
      - The successor's own timeline can therefore receive the input's values.
        `SampleRealtimePortStateStorageView::feedback_timeline` marks a view of a
        feedback timeline whose writer is a copy or a composition. Only its
        connection's input reads that storage. It is set in
        `plan_sample_realtime_port_state_realizations()`.
      - `sample_source_pairs_by_producer()` is renamed `sample_source_views_pair()`
        and also pairs two feedback timeline views, without requiring the same read
        delay. `sample_sources_pair_by_producer()` is renamed
        `sample_sources_pair()`.
      - Such an input now takes per-source `transfer_to_steady_storage` transfers.
        It needs no transition root, no steady memory and no steady time. The
        cutover copies `c` values per timeline between audio callbacks.
        `resolve_sample_transfer_conflicts()` still moves the record to
        transition-only storage when two records disagree on a shared timeline
        slot.
    - *Fourth collaborator run (2026-11-08): all 811 tests pass.*
    - *Reproduction tests for two remaining gaps (2026-11-08; not yet run).* Both
      need no production change. The new nodes and modules are at the end of
      package part 1.
      - `GrowingDetachLatencyReadsNeutralValuesAfterInFlightValues`.
        - `NeutralFeedbackCounter` adds one to its input, whose authored
          `neutral_value` is 100. `neutral_feedback_counter_module` feeds its
          output back through a `detach()` with no initial value. By the
          `detach()` contract (node_ref_dsl_contract.md), that storage starts at
          the input's neutral value. Its initial value is not zero, so the loop
          uses a `copy` feedback timeline rather than the producer's home ring.
        - The edit lengthens the `detach()` from 5 to 7. The input's window
          `[0, 7)` inherits `[0, 5)`; positions 5 and 6 are fresh.
        - The test expects those two positions to read 100, as the loop's first
          positions do. Expected to fail: `execute_realtime_port_state_transfers()`
          sets every fresh position to zero (`std::memset`). The counter therefore
          outputs 1 instead of 101 there, and every value derived from them
          differs.
      - `RewiredChannelBesideDetachedChannelReadsItsNewSource`.
        - `RoutedHistoryProbe` has a stereo input with 3 values of history.
          `routed_rewire_module` feeds the probe's output back to the left channel
          through `detach(4)`, and connects a ramp to the right channel. The edit
          rewires the right channel to the offset ramp.
        - Because the two channels have different detach settings, the connection
          is routed. Its final `SampleCompositionPlan` runs before the consumer
          (`before_execution_position`).
        - The right channel's record is composed. Its predecessor and successor
          sources are different producers, so they do not pair, and the record
          moves to transition-only storage.
        - `specialize_transition_plan()` (lowering.cpp) then redirects that input
          channel to a private suffix ring. It adds a `SampleMaterializationPlan`
          (the local `mirror`) that copies the composition's target into that
          ring. Because the composition runs before the consumer, it appends that
          materialization to the consumer step's own
          `sample_transition_materializations_after`, which runs after the
          consumer's callback. The callback therefore reads ring positions that
          the copy has not yet written.
        - Expected to fail with 0 at every right-channel position of the
          transition root's block.
    - *Fifth collaborator run (2026-11-08): 811 of 813 tests pass.* Both new tests
      fail as predicted.
      - `GrowingDetachLatencyReadsNeutralValuesAfterInFlightValues`: positions 1029
        and 1030 read 1 instead of 101, with 292 mismatches in all.
      - `RewiredChannelBesideDetachedChannelReadsItsNewSource`: every right-channel
        read in the transition root's block, 1024 to 1087, is 0.
    - *Decision (human co-author, 2026-11-08).* A fresh position holds what freshly
      initialized storage would hold there. Behind a `detach()` this is its initial
      value: the authored one, otherwise the reading input's neutral value.
      Elsewhere it is the input's `neutral_value`. Recorded in
      sequential_port_storage_planning.md.
    - *Fixes (2026-11-08; collaborator build and test run pending).*
      - *Fresh value.*
        - `SampleRealtimePortStateStorageView::initial_value` holds the
          persistent allocation's `initialize_value`. Feedback timelines declare
          their `detach()` initial value, and slot-fed input timelines declare
          the neutral value.
        - `SampleRealtimePortStateRealization::neutral_value` holds an input's
          neutral value. It comes from the new
          `initialized_sequential_input_value()` (lowering_plan.cpp), which the
          input-timeline and `detach()` default paths now also use.
        - `SampleRealtimePortStateTransfer::fresh_value` is chosen by
          `sample_fresh_value()`: the target's `initial_value`, otherwise the
          successor input's neutral value, otherwise zero for a Tick output. A
          transition-only target takes the value of the steady storage it stands
          in for.
        - `execute_realtime_port_state_transfers()` copies `fresh_value` where it
          used `std::memset`. Preparation checks its size.
        - In `resolve_sample_transfer_conflicts()`, fresh slots of two claims agree
          only when their fresh values are equal.
        - The two hand-built transfers in realtime_port_state_transition_gtest.cpp
          now carry a zero `fresh_value`.
      - *Copy order.* The new `PrimitiveExecutionStep::
        sample_transition_materializations_before` runs after a step's
        `sample_compositions_before` and before its callback. When the input's
        steady representation is a composition with `before_execution_position`,
        `specialize_transition_plan()` places the input's
        `SampleMaterializationPlan` there. Steps with that list stay unbatched,
        like steps with any other list.
    - *Sixth collaborator run (2026-11-08): all 813 tests pass.*
    - *Reproduction tests for the remaining gaps (2026-11-08; not yet run).* None
      needs a production change. Their nodes and modules are at the end of
      package part 1. Each expects the behavior that
      sequential_port_storage_planning.md requires.
      - `RewiredComposedChannelReadsItsInFlightValuesFirst` and
        `RewiredConvertedInputReadsItsInFlightValuesFirst` (gap: a moved
        converted or composed input after a rewire).
        - An interleaved stereo input composes its left channel from the ramp,
          compensated by 7 for the delay node on its right channel. In the
          second test, an interleaved mono input reads the ramp through a layout
          conversion, compensated by 7 for a slower input.
        - The edit rewires the ramp read to the offset ramp, whose latency is the
          same. The tests expect the 7 in-flight ramp values, then the offset
          ramp.
        - Expected to fail at those 7 positions. The sources do not pair, so each
          record moves to transition-only storage. The `SampleMaterializationPlan`
          that copies the steady representation into the suffix ring copies at
          the input's read delay, so it overwrites them.
      - `GrowingSourceLatencyExposesNeutralValues` (gap: transition-only input
        whose producer's latency grew).
        - `NeutralSequenceProbe`, whose input's neutral value is 100, reads a ramp
          whose latency grows from 0 to 3. The test expects 3 positions of 100,
          then the ramp 3 values late.
        - Expected to fail when compiling the transition. The ramp's new output
          window is a new record whose fresh value is zero. The input's 3 fresh
          positions lie in the same ramp storage slots and hold 100, so the two
          disagree and `resolve_sample_transfer_conflicts()` moves the input to
          transition-only storage. Its window ends at `T = 3`, beyond its
          compensation of 0, so `specialize_transition_plan()` reports "cannot
          yet move an input whose producer's latency grew".
      - `CompensatedInputReadsItsNeutralValueBeforeItsSourceStarts` (gap: fresh
        start).
        - One generation. `NeutralCompensatedSequenceProbe`'s ramp input, whose
          neutral value is 100, is compensated by 7. The test expects its first 7
          positions to read 100.
        - Expected to fail with 0 there: the ramp's storage starts zero-filled.
      - `ConnectedInputReadsNeutralValuesBeforeItsNewSource` (gap: no predecessor
        record).
        - `compensated_sequence_probe`'s ramp input is disconnected before the
          edit and connected to the ramp, compensated by 7, after it. A history
          reader keeps the ramp's recent values in its storage. The test expects
          7 positions of the input's neutral value 0, then the ramp.
        - Expected to fail with the ramp's values from before the cutover at
          those 7 positions.
        - The slot-fed case has the same cause: an input without history that is
          not fed only through realtime storage gets no record. It has no test,
          because a slot-fed input needs a background-produced source.
      - `GrowingCompensationOfComposedChannelExposesNeutralValues` (gap: composed
        input whose window inherits nothing).
        - An interleaved stereo input composes its left channel from the ramp and
          its right from a second ramp. The edit moves the right channel to the
          delay node, so the left channel's compensation grows from 0 to 7. A
          history reader keeps the ramp's values.
        - The test expects 7 positions of the neutral value 0, then the ramp 7
          values late. Expected to fail with the ramp's values from before the
          cutover there, because `apply_input_window()` leaves the record
          `fresh_state`.
      - `UnequalLatencyFeedbackUnaffectedByAddedLeadingNode` (gap: raw
        identities with bundle handles).
        - `detach_feedback_module`, rewritten by the new
          `project_unequal_latency_sample_feedback()`, is the unequal-latency
          routed loop, but its nodes have stable identities. The fixture
          `configured_unequal_latency_projected_sample_feedback_graph()` now
          applies the same function.
        - The edit adds a node that nothing reads before the loop, which
          renumbers its bundles. The test expects the converted node's reads to
          match an uninterrupted run.
        - The loop's alignment ring holds the older source frames that the
          timeline writer mixes in after the cutover. No record covers it. Its
          identity, `routed_feedback_identity()` plus `:alignment=`, contains
          bundle handles, so it should not migrate. Expected to fail in the calls
          shortly after the cutover.
        - The routed timeline and the converted node's canonical storage also
          have identities with bundle handles. The writer's revision recopies
          source frames from that storage.
      - Helpers: `expect_converted_feedback_unaffected_by_edit()` generalizes the
        recompile check to two graphs. `expect_indexed_probe_values_across_graph_edit()`
        and `expect_probe_values_across_graph_edit()` check probe values without
        checking a read latency.
    - *Seventh collaborator run (2026-11-09): 816 of 820 tests pass.*
      - Failed:
        - `GrowingSourceLatencyExposesNeutralValues`: positions 1024 to 1026 read
          0 instead of 100. These were steady blocks: the transition compiled
          with no transition root, so no compile error.
        - `CompensatedInputReadsItsNeutralValueBeforeItsSourceStarts`: positions
          0 to 6 read 0 instead of 100.
        - `ConnectedInputReadsNeutralValuesBeforeItsNewSource`: positions 1024 to
          1030 read 1017 to 1023, the ramp's values from before the cutover. The
          transition root was also still active after the run's 4 blocks.
        - `UnequalLatencyFeedbackUnaffectedByAddedLeadingNode`: the converted
          node's reads differ from the uninterrupted run from the call at 68 on.
      - Passed, against the prediction: `RewiredComposedChannelReadsItsInFlightValuesFirst`,
        `RewiredConvertedInputReadsItsInFlightValuesFirst` and
        `GrowingCompensationOfComposedChannelExposesNeutralValues`.
    - *Why three predictions were wrong (2026-11-09).* The three tests that passed
      reach `direct` records, not `composed` or `materialized` ones.
      - A conversion whose every target channel copies exactly one source channel
        passes `can_alias_sample_conversion()` (sample_storage_plan.cpp). Each
        target channel then binds directly to its source's storage through a
        `SampleChannelBindingPlan` whose `frame_delay` is the source's read
        latency.
      - Both a `tile<iv::stereo>` into an interleaved stereo input and a mono
        source into an interleaved mono input convert this way. Their records
        are `direct`, a path that already worked.
      - A `materialized` record needs a computed conversion of one producer: a
        `SampleMaterializationPlan` into transient storage, for example a stereo
        source read by a mono input. A `composed` record needs a computed
        conversion of several producers: a `SampleCompositionPlan` into
        transient storage, for example a `tile<iv::stereo>` read by a mono
        input.
      - The three tests stay, renamed for what they reach:
        - `RewiredTiledChannelReadsItsInFlightValuesFirst` (module
          `tiled_rewire_module`);
        - `RewiredInterleavedMonoInputReadsItsInFlightValuesFirst`
          (`interleaved_mono_rewire_module`);
        - `GrowingCompensationOfTiledChannelExposesNeutralValues`
          (`tiled_compensation_module`).
    - *Causes of the four failures (2026-11-09; from the source, not yet confirmed
      by a run).*
      - `GrowingSourceLatencyExposesNeutralValues`.
        - Before the edit nothing retains the ramp's values. The probe's input
          reads them only while its callback runs, so its record is
          `callback_only`.
        - `input_resume_position()` recognizes "the same producer" only between
          two `direct` records. Here it falls back to `T = c_new = 0`, so
          `apply_input_window()` gives an empty window and `fresh_state`.
        - The ramp's successor storage is new and zero-filled, so the probe reads
          0. By the rule, the probe holds the ramp's values up to the cutover, so
          `T = c_old + d_new - d_old = 3`.
        - The test now also checks that the probe's input record is
          `callback_only` before the edit.
        - The "latency grew" error predicted for it needs a retained source. The
          new `GrowingLatencyOfRetainedSourceExposesNeutralValues` adds one.
      - `CompensatedInputReadsItsNeutralValueBeforeItsSourceStarts`, as predicted:
        `NodeStorage::initialize()` zero-fills the ramp's storage, and the probe
        reads its frames -7 to -1.
      - `ConnectedInputReadsNeutralValuesBeforeItsNewSource`, as predicted.
        - The disconnected input has no predecessor record. In
          `resolve_sample_transfer_conflicts()`, the new record's fresh claim
          (frames `[P - 7, P)`, value 0) disagrees with the history reader's claim
          on the ramp's storage.
        - So the history reader's transfer is the one moved to transition-only
          storage, carrying 5000 values. Its expiry explains the transition root
          still active after 4 blocks.
        - No transfer writes the new record's fresh value, so the probe reads the
          ramp's storage as it was.
      - `UnequalLatencyFeedbackUnaffectedByAddedLeadingNode`: the edit only
        renumbers bundles, and the same graph recompiled without renumbering
        passes `UnequalLatencySampleFeedbackMigratesPopulatedAlignmentRing`. So
        the failure comes from a raw identity built from bundle handles. Which
        region, whether the alignment ring alone or also the routed timeline, is
        not yet established.
    - *Reproduction tests that reach `materialized` and `composed` records
      (2026-11-09; not yet run).*
      - New in package part 3: the node `OffsetStereoRampSource`, and the modules
        `downmixed_rewire_module`, `downmixed_tile_rewire_module`,
        `downmixed_compensation_module`, `downmixed_tile_compensation_module` and
        `retained_neutral_latency_module`.
      - The probe is `ConvertedSequenceProbe`, whose `in` is interleaved mono and
        whose `slower` sets its compensation.
      - `expect_sequential_input_access()` compiles a graph and checks the
        `retained_access` of its one Sequential input record with a given history
        and in-flight count. Each test below checks the record it targets before
        running the edit.
      - `RewiredDownmixedInputReadsItsInFlightValuesFirst` (`materialized`).
        - `in` downmixes the stereo ramp (value `p + 500`), compensated by 7. The
          edit rewires it to the offset stereo ramp (`p + 100500`).
        - Expects 7 in-flight values from the stereo ramp, then the offset stereo
          ramp.
        - Expected to fail at those 7 positions. The window maps them to frames
          `[P - 7, P)` of the suffix ring. The `SampleMaterializationPlan` that
          `specialize_transition_plan()` adds copies the steady materialized
          storage at `mirror_delay = 7`, so it overwrites them with the new
          downmix.
      - `RewiredDownmixedTileReadsItsInFlightValuesFirst` (`composed`).
        - `in` downmixes `tile<iv::stereo>(ramp, second ramp)`, compensated by 7.
          The edit rewires the right channel to the offset ramp.
        - Expects 7 in-flight values, then `p - 7 + 50000`.
        - Expected to fail at those 7 positions. A feed-forward composition
          writes consumer-aligned frames, and its consumer reads at delay 0
          (lowering_plan.cpp). So `mirror_delay = 0`, and the copy overwrites
          frames `[P, P + 7)`, where the window holds the in-flight values.
      - `GrowingCompensationOfDownmixedInputExposesNeutralValues` (`materialized`)
        and `GrowingCompensationOfDownmixedTileExposesNeutralValues` (`composed`).
        - The edit moves `slower` to the delay node, so `in`'s compensation grows
          from 0 to 7. History readers keep the sources' storage populated.
        - Expect 7 fresh positions of the neutral value 0.
        - Expected to fail with the sources' values from before the cutover.
          `apply_input_window()` leaves a record that inherits nothing and is not
          `direct` as `fresh_state`, and the steady downmix reads the sources 7
          frames back.
      - `GrowingLatencyOfRetainedSourceExposesNeutralValues` (`direct`).
        - `GrowingSourceLatencyExposesNeutralValues` with a `history_consumer` on
          the ramp, so the probe's input is `direct` before the edit.
        - Expected to fail compiling the transition with "cannot yet move an input
          whose producer's latency grew".
        - The probe's fresh claim (100) disagrees with the history reader's fresh
          slots (0) and with the ramp's new output record (0), so the probe moves
          to transition-only storage. Its window ends at `T = 3`, beyond its
          compensation of 0.
    - *Eighth collaborator run (2026-11-09): 816 of 825 tests pass.* All nine
      reproduction tests fail, each where predicted.
      - `GrowingSourceLatencyExposesNeutralValues`: 1024 to 1026 read 0 instead
        of 100, in steady blocks.
      - `CompensatedInputReadsItsNeutralValueBeforeItsSourceStarts`: 0 to 6 read 0
        instead of 100.
      - `ConnectedInputReadsNeutralValuesBeforeItsNewSource`: 1024 to 1030 read
        1017 to 1023, and the transition root was still active after 4 blocks.
      - `RewiredDownmixedInputReadsItsInFlightValuesFirst`: 1024 to 1030 read 0
        instead of 1517 to 1523.
      - `RewiredDownmixedTileReadsItsInFlightValuesFirst`: 1024 read 508.5, which
        is (1017 + 0) / 2: the rewired channel's in-flight value was overwritten
        by the offset ramp's zero-filled storage.
      - `GrowingCompensationOfDownmixedInputExposesNeutralValues` and
        `GrowingCompensationOfDownmixedTileExposesNeutralValues`: 1024 read 1517
        and 1017 instead of 0.
      - `GrowingLatencyOfRetainedSourceExposesNeutralValues`: the transition
        compile failed with "cannot yet move an input whose producer's latency
        grew to transition-only storage".
      - `UnequalLatencyFeedbackUnaffectedByAddedLeadingNode`: as in the seventh
        run.
    - *Fixes (2026-11-09; collaborator build and test run pending).* The human
      co-author approved the fixes proposed after the seventh run, except where
      one is not cheap; the one such case is listed under the known gaps.
      - *Copy into transition-only storage.* The `SampleMaterializationPlan`
        that `specialize_transition_plan()` adds (the local `mirror`) copies
        frames `[S - mirror_delay, S + block - mirror_delay)` (see
        `emit_sample_materialization()`). The window holds positions
        `[-history, T)`, which end at frame `P + T - d`.
        - For a `direct` record, `mirror_delay` is now `d - min(T, in_flight)`.
          When `T <= in_flight` this equals the former `d - T`, and the first copy
          begins where the window ends.
        - When the same producer's latency grew (`T > in_flight`), `d - T` would
          copy values the producer can still revise. `d - in_flight` copies only
          finalized values, and lowering no longer reports an error.
        - A `materialized` or `composed` steady representation holds only the
          frames its consumer reads in the current block, so `mirror_delay`
          stays `d`.
        - Wherever the first copy would begin inside the window, the mirror skips
          the frames before `P + T - d`. The new
          `SampleRealtimePortStateCutoverIndex` names an 8-byte transition-only
          cell for each Sequential pace cursor of a transition-only input.
          `realize_realtime_port_state_transition_storage()` declares it, and
          `execute_realtime_port_state_transfers()` copies the cursor into it.
          `SampleMaterializationPlan::transition_cutover_index_storage_offset`
          and `transition_first_frame` make the copy load the cell and start at
          `max(S - mirror_delay, cell + transition_first_frame)`: one load, one
          subtraction and one select per block of the transition root.
      - *Same producer.* `input_resume_position()` recognizes the same producer
        through `input_single_producer()`: the one producer that a `direct`
        record reads, that a `callback_only` record's `authored_storage` reads,
        or that every source of a `materialized` or `composed` record reads at
        one timeline offset. The producer's channel no longer matters, because a
        producer's channels finalize together.
      - *Windows that inherit nothing.* `fresh_window_realization()` gives a
        window of only fresh positions fresh-only transfers into each storage
        view the input reads: the one view of a `direct` record, or each source
        view of a `materialized` or `composed` one. Before, only `direct` records
        got one. Each transfer's value is the input's own fresh value in that
        view. Per-source fresh values compose to the input's fresh value (see
        the 2026-11-08 note on registered conversions). A constant source view
        gets none.
      - *Inputs without a predecessor.* `make_fresh_input_transition()` gives a
        Sequential input that the predecessor does not have its own transition,
        new or anonymous. `RealtimePortStateTransition::has_predecessor` is false,
        and its whole window `[-history, in_flight)` is fresh: sequential_port_
        storage_planning.md forbids fabricating its history from source storage
        that happens to hold values.
        - Its fresh-only transfers take part in `resolve_sample_transfer_conflicts()`
          like any other state, so when they disagree with another state, the new
          input is the one moved.
        - `build_steady_storage_transfers()` and
          `realize_realtime_port_state_transition_storage()` read no predecessor
          view for it.
        - An input that reads a constant, or callback-local bytes, gets no
          transition. *Collaborator interpretation, not yet confirmed:* a
          constant's storage holds the constant at every position, so it is what
          freshly initialized storage would hold there. Surviving inputs that
          read a constant already behaved this way.
      - *Fresh start.* `plan_fresh_sample_storage_values()` plans a transition
        from an empty predecessor and returns each persistent sample region
        where every state with a window agrees on one fresh value that is not
        zero bytes. A Tick output's own storage counts as zero.
        - A reader reads a frame before its source writes it only inside its own
          window. So for such a region, initializing the whole region with that
          value is the same as initializing each window.
        - `declare_fresh_sample_storage_values()` (lowering_plan.cpp) runs after
          realization planning. It installs the value as the region's raw
          initializer through `declare_sample_fresh_storage_value()`
          (sample_storage_plan.cpp). It also sets the value as the
          `initial_value` of every view of that region.
        - `NodeStorage::initialize()` runs a raw initializer only for a region
          that was not restored by migration. So this costs nothing at run time
          and does not change a migrated region.
      - *Nominal raw identities.* The new `raw_region_node_identity()`
        (connection_plan.cpp) gives the node part of a compiler-owned raw-region
        identity: `{` + `migration_identity_fields()` + `}` for a node with a
        `StableConcreteNodeId`, otherwise its bundle handle as before. It replaces
        the bundle handles in:
        - `persistent_identity`, `feedback_identity`,
          `composition_feedback_identity`, `routed_feedback_identity` (and so its
          `:alignment=` ring), and `composition_feedback_alignment_identity`
          (sample_storage_plan.cpp);
        - the sample `disconnected_output` sink, `event_group_identity`, the event
          `raw_output` identity, and both event producer-ring identities
          (lowering_plan.cpp).
      - *Changed test.* `NewStateSharingSampleTargetRequiresTransitionStorage`
        (realtime_port_state_transition_gtest.cpp) encoded the old rule, under
        which a new state's fresh claim moved the surviving state. It is now
        `NewStateSharingSampleTargetMovesItselfToTransitionStorage`.
        - The survivor keeps its steady transfer into the shared target, and the
          new state moves to transition-only storage with fresh zeros.
        - The test also checks that one cutover index cell receives the cursor
          value.
      - Expected effect: every reproduction test above passes. Edits that add an input with
        history or compensation to storage that other states already hold now
        need a transition root, which costs one transition compile per such edit.
    - *Ninth collaborator run (2026-11-09): all 825 tests pass.* No test
      reached the gaps listed below, which is why they still pass.
    - *Reproduction tests for the remaining gaps (2026-11-09; not yet run).* Their modules
      are at the end of package part 1, which is now 58,959 of 61,440 bytes.
      - `CompensatedInputBesideHistoryReaderReadsItsNeutralValueBeforeItsSourceStarts`
        (gap: fresh start where two states disagree in one region).
        - `shared_neutral_compensated_module`: `NeutralCompensatedSequenceProbe`'s
          ramp input (neutral value 100), compensated by 7 for a slower input
          that reads a ramp of latency 7. A `LargeHistoryConsumerProbe`
          (neutral value 0, history 5000) reads the same ramp.
        - Expects the probe's first 7 positions to read 100 and the history
          reader's position 5000 back to read 0.
        - Expected to fail with 0 at the probe's first 7 positions. The two
          windows overlap in frames -7 to -1 of the ramp's storage with
          different fresh values, so `plan_fresh_sample_storage_values()` leaves
          that region at zero bytes.
      - `GrowingCompensationOfComposedSourcesWithUnequalLatencies` (gap: a
        composed input whose sources resume at different positions).
        - `unequal_latency_composition_module`: `ConvertedSequenceProbe`'s mono
          input downmixes `tile<iv::stereo>` of a ramp of latency 3 (A) and the
          offset ramp (B). Its slower input reads a ramp of latency 7, raised to
          10 by the edit. A's compensation grows from 4 to 7 and B's from 7 to
          10. The record is `composed` with 7 in-flight values before the edit
          and 10 after; the test checks both.
        - By sequential_port_storage_planning.md applied to each source, A keeps
          its 4 in-flight values, exposes 3 fresh positions and continues from
          position 7. B keeps 7, exposes 3 and continues from position 10. A
          fresh position of a source contributes half of the neutral value 0.
        - Expected to fail at positions `C + 4` to `C + 9`. `input_resume_position()`
          gives the record `T = c_new = 10`, so the window inherits the old
          downmix at positions 0 to 6, including A's values at 4 to 6. It holds
          fresh values for both sources at 7 to 9.
      - `AddedFeedbackChannelReadsItsDetachValuesBesideItsContinuingChannel`
        (gap: channels sharing one suffix ring with different window ends).
        - `added_feedback_channel_module`: `RoutedHistoryProbe` (stereo input,
          history 3) reads a ramp on each channel. The edit feeds its left
          channel from its own output through `detach(4)`, which routes the
          connection. Both channels' records are then `composed`, with 4 and 0
          in-flight values; the test checks both. An observer
          (`SampleSequenceProbe`) reads the probe's output, which is the left
          channel plus one.
        - Expects the right channel to continue its ramp. The left channel
          should read the detach's initial value (the input's neutral value 0)
          for 4 positions, then the probe's output 4 values late, so the
          output is `1 + (p - C) / 4` from the cutover.
        - Expected to fail compiling the transition with "GraphJit transition
          input suffix mirrors channels read at different delays". Neither
          channel pairs (`direct` before, `composed` after), so both move into
          one suffix ring. The left window ends at 4, the right at 0, so only
          the left's mirror skips frames.
      - *Tenth collaborator run (2026-11-09): 825 of 828 tests pass.* The three
        reproduction tests above fail as predicted.
        - `CompensatedInputBesideHistoryReaderReadsItsNeutralValueBeforeItsSourceStarts`:
          positions 0 to 6 read 0 instead of 100. The history reader's position
          5000 back reads 0, as expected.
        - `GrowingCompensationOfComposedSourcesWithUnequalLatencies`: exactly
          positions 1028 to 1033 differ. At 1028 to 1030, 51021 to 51023 are
          read instead of 50510.5 to 50511.5 (the old downmix, including the
          latent ramp's values). At 1031 to 1033, 0 is read instead of 510.5 to
          511.5 (both sources fresh).
        - `AddedFeedbackChannelReadsItsDetachValuesBesideItsContinuingChannel`:
          the transition compile reports "GraphJit transition input suffix
          mirrors channels read at different delays".
      - *Decision (human co-author, 2026-11-09).* A fresh start whose states
        disagree in one region uses option A. Only the first generation's
        compile becomes a cutover from an empty graph, with a transition root.
        `GraphExecutor` runs its transfers when it initializes the first
        generation and starts it in the transition phase. A no-predecessor
        compile that the manual-control recompile requests stays steady-only.
        The request must therefore tell the two apart. The other options are
        rejected for their behavior or cost:
        - private steady storage for one of the readers would copy every block
          to fix startup;
        - an exception to the fresh-value rule.
      - `CompatibleCodeReplacesSteadyCodeOfGenerationWithTransitionRoot`
        (gap: a realtime-code replacement for a generation compiled with a
        transition root).
        - Intent:
          - node_presentation_and_manual_controls_direction.md: dynamic and
            constant code for one structural realization share its canonical
            `NodeLayout`, and `stage_realtime_code()` changes the steady Tick
            operations;
          - `ExecutionGeneration::realtime_code` (graph_executor.h): a
            compatible steady-code replacement changes only the operations used
            after the transition phase;
          - `activate_pending()` swaps the code only once the phase is `steady`.
        - The test drives `GraphExecutor` with `downmixed_rewire_module`:
          - the original graph as the first generation;
          - its rewired successor compiled as a cutover with a transition root;
          - the rewired graph compiled again without a predecessor, as the
            manual-control recompile does, staged through `stage_realtime_code()`.
        - Expects it accepted, not active while the transition root runs, and
          active after the next block.
        - Expected to fail: `compatible_realtime_code()` compares the whole
          `storage_size` and every region, including the structural graph's
          transition-only suffix, so `stage_realtime_code()` throws
          `std::invalid_argument`.
      - *Eleventh collaborator run (2026-11-09): 825 of 829 tests pass.*
        `CompatibleCodeReplacesSteadyCodeOfGenerationWithTransitionRoot` fails as
        predicted: `stage_realtime_code()` throws `std::invalid_argument` ("GraphExecutor
        realtime-code replacement is not layout-compatible"). The other three
        fail as in the tenth run.
      - *Decisions (human co-author, 2026-11-09).*
        - Compiler-owned storage of a node under no virtual node is not
          preserved either. Such a node behaves like a node the edit adds. For
          example, consumers then read zero bytes, not the old node's values, in
          its output latency window.
        - A `detach()` is a first-class structure, not a node. It is called on a
          node or a node's port in the source. Its structures (timelines,
          alignment rings) are identified across graphs in this order:
          - by its producer port, when that port has an identity;
          - then by its consumer port, when that port has an identity and the
            producer port is missing or does not tell the detach apart;
          - then by its latency and initial value.
        - The collaborator's earlier claim, that an anonymous consumer's detach
          loop would restart its alignment ring, does not respect the structure
          of C++ node graphs and is withdrawn.
        - Both follow from one rule already stated in
          sequential_port_storage_planning.md: observable state belongs to
          whatever owns it, and survives an edit exactly when that owner is
          identified in both graphs. Storage only serves that rule.
          - A node's `State` and its ports' windows belong to the node.
          - A Tick output's storage belongs to its producer port.
          - A detach's structures hold values derived from what its producer
            produced, so its producer port identifies them first.
          - The values an input's read delay has not yet let it read belong to
            that input, so an anonymous or new consumer of a surviving detach
            starts its own window fresh, which the code already does.
          - Questions of storage identity are answered by this rule, not by a
            new decision.
        - Not yet implemented; reproduction tests come first.
        - `AnonymousFeedbackLoopStartsFreshAtAnEdit` (not yet run).
          - `configured_unequal_latency_projected_sample_feedback_graph()` builds
            the unequal-latency routed loop without source identities. The test
            checks that none of its nodes has a stable identity, so neither has
            its detach.
          - Activating a recompile at index 64 must then behave like the same
            graph started fresh at index 64: the converted node's reads, which
            restart with its `State`, equal those of a fresh run from 64.
          - Expected to fail, though not certain, in the converted node's reads
            after its first 6 positions. The loop's alignment ring and routed
            timeline still migrate by bundle handle, so the timeline writer mixes
            source frames from before the edit into those reads.
          - Implementing the rule will also move the six tests listed below onto
            nodes with identities, since they test migration itself.
      - *Twelfth collaborator run (2026-11-09): 829 of 830 tests pass.* Every
        earlier fix passes. `AnonymousFeedbackLoopStartsFreshAtAnEdit` fails as
        predicted: the first call after the edit matches the fresh run, then from
        the call at 68 on the converted node reads values of the old loop (for
        example 8.18 instead of 0.25).
      - *Identity rule (2026-11-09; collaborator build and test run pending).*
        - `raw_region_node_identity()` (connection_plan.cpp) now returns nothing
          for a node under no virtual node.
          `raw_region_node_identity_text()` puts a marker in its place, and
          `owned_raw_region_identity()` declares an identity that contains the
          marker empty, so such a region never migrates. Every compiler-owned
          sample and event region passes through it where it is declared:
          `append_persistent_allocation()`,
          `append_synthetic_persistent_allocation()` and the event
          `append_representation()`.
        - `resolve_detach_identities()` gives each detach the shortest key no
          other detach of the graph shares: `producer=` its detached source
          ports, then also `:consumer=` its consumer port, then also
          `:settings=` its latency and initial value. A part is present only
          when every node it names has an identity. A detach that nothing
          tells apart gets no identity.
          - `SampleDetachIdentities` (sample_storage_plan.cpp) resolves every
            whole-connection detach and every routed contribution's detach once
            per plan. A routed contribution's consumer part also names its
            target channels.
          - `feedback_identity`, `composition_feedback_identity`,
            `routed_feedback_identity` (with its `:alignment:capacity=` ring)
            and `composition_feedback_alignment_identity` now hold that key plus
            only the shape of their bytes: layout, value type, pace, kind and
            capacity.
          - Latency, history, read latencies and initial value left them. A
            detach whose latency changes therefore keeps its structures when its
            producer port identifies it, and a region whose shape changed does
            not migrate, because raw migration skips a size mismatch.
        - An event detach's storage is shared by every consumer of the same
          source, latency and retention. Its identity (producer group, source
          position, latency) already orders producer first; a consumer cannot
          identify a shared structure, so it starts fresh when its producer is
          anonymous.
        - Tests moved onto nodes with identities:
          - `with_node_identities()` (graph_jit_gtest.cpp) puts each concrete
            node of a built graph in a virtual node of its own.
          - The six migration tests use it. In
            `UnequalLatencySampleFeedbackMigratesAlignmentPrehistory` and
            `...PopulatedAlignmentRing`, `expect_converted_feedback_suffix_equal()`
            already accepts a node `State` that migrates.
          - `ExactSampleDetachFeedback`, `MultipleSampleDetachBranchesMigrateSharedHomeAndFallbackCopy`,
            `ExactTypeEventDetachFeedback` and `FeedForwardEventFanInCompactCarryMigrates`
            now expect the nodes' `State` to continue: the calls after the edit
            follow the earlier calls in the same slots. Their nodes' outputs do
            not depend on `State`, so the values are unchanged.
          - `configured_event_feedback_scc_external_fanout_graph()` and
            `configured_staged_event_fan_in_graph()` return graphs with
            identities, because their tests locate regions by migration
            identity.
          - The three hand-built `GraphJitSampleStoragePlan` tests add identified
            `PlannedGraphNode` entries through `add_identified_test_nodes()`.
          - `sample_feedback_alignment_regions()` matches `:alignment:`.
      - *Thirteenth collaborator run (2026-11-09): 828 of 830 tests pass.*
        `AnonymousFeedbackLoopStartsFreshAtAnEdit` and the six migration tests
        pass.
        - `EventDetachFeedbackFanoutSharesDelayedStream` and
          `EventDetachFeedbackRetainsBurstAcrossTinyBlocks` failed: they locate
          the `graphjit.event.feedback:` region by migration identity on
          anonymous builder graphs, whose identities are now empty.
        - `configured_event_feedback_fanout_graph()` and
          `configured_event_feedback_graph()` now return graphs with identities,
          like the other two fixtures.
        - Every other test that matches migration identities uses module graphs,
          one of those four fixtures, hand-built analyses with
          `add_identified_test_nodes()`, or a producer group without source
          channels, whose identity names no node.
      - *Fourteenth collaborator run (2026-11-09): all 830 tests pass.*
      - *Decision (human co-author, 2026-11-09).* The intended observable
        behavior is implemented before any further optimization.
      - *Remaining observable gaps (2026-11-09).* A search of the normative docs
        and this record finds two, both in graph-revision continuity.
        - A converted or composed input that a rewire or a conflict in shared
          storage moves to transition-only storage keeps one window for the
          composed value, at the input's `T`. When its sources resume at
          different positions, the window holds contributions its sources had
          not yet finalized.
        - An event input owns no in-flight events.
          - sequential_port_storage_planning.md ("Port windows are semantically
            node-owned") gives every Sequential input its history and its
            in-flight values. graph_jit_direction.md repeats this ("each
            Sequential input's resolved history and in-flight values").
          - An event connection's only read delay is a `detach()` latency. But
            `inventory_realtime_port_states()` gives an event input a record for
            its history only, and `inventory_sequential_input_in_flight_values()`
            handles sample connections only.
          - The triggers a detach has not yet delivered therefore live only in
            its feedback ring. They survive an edit only when that ring migrates
            by raw identity, and the event feedback identity includes the
            latency, the producer group and the retention.
          - So a latency change, a rewire, or an anonymous producer loses them.
      - *Reproduction tests (2026-11-09; not yet run).* Their modules are at the
        end of package part 1, now 60,709 of 61,440 bytes.
        - `RewiredComposedInputReadsItsLatentSourceFinalValues`.
          - `revising_tile_rewire_module`: `ConvertedSequenceProbe`'s mono input
            downmixes `tile<iv::stereo>` of `RevisingSampleSource` and the ramp,
            compensated by 7 for the delay node. `RevisingSampleSource` has
            latency 1 and revises each value one position later to its final
            value, `position + 100`. The edit rewires the ramp to the offset
            ramp.
          - The record is `composed` with 7 in-flight values in both graphs.
          - Each source keeps its own in-flight values. The revising source
            owns 6 final ones and resumes at 6 with the value it finalizes after
            the cutover, and the ramp plays its 7 in-flight values first.
          - Expected to fail at position `C + 6` only. The rewired record moves
            to transition-only storage with one window at `T = 7`. It inherits
            the old downmix there, which holds the revising source's frame
            `C - 1` before its revision (1023 instead of 1123).
        - `GrowingEventDetachDeliversItsInFlightEventsFirst`.
          - `event_feedback_module(latency)`: `EventFeedbackA` and
            `EventFeedbackB` in a loop, as in `ExactTypeEventDetachFeedback`, but
            with identified nodes. B reads A's triggers through
            `detach(latency)`. Each node sends one trigger per 8-sample slice.
            The edit at 64 lengthens the detach from 10 to 12.
          - Expects B to receive A's trigger sent at 57 at 67, its in-flight
            value. Positions 74 and 75 are fresh, and later triggers arrive 12
            after they are sent (77, 85, …).
          - Expected to fail at the slice at 64, with no trigger, or earlier,
            in the transition compile. The ring that held the trigger has a new
            identity and starts empty, and no record carries the trigger.
      - *Fixes (2026-11-09; collaborator build and test run pending).*
        - *Realtime-code replacement.* `compatible_realtime_code()` compares a
          replacement with the structural generation's steady prefix only.
          - It checks the replacement's `storage_size` against its own
            `steady_storage_size`, which equals the structural one.
          - It checks the structural generation's non-transition-only regions,
            which `NodeLayoutTransitionBuilder` puts first.
          - It accepts a structural `storage_alignment` that is a multiple of the
            replacement's, since a transition-only region may raise it.
          - The replacement still takes over only once the phase is `steady`.
        - *Separate suffix rings.* In
          `realize_realtime_port_state_transition_storage()`, channels share a
          suffix ring only when their windows end at the same position and have
          the same timeline offset and in-flight count:
          `SuffixTargetGroup::window_end` and `in_flight`. Then one mirror, with
          one delay and one first frame, serves every channel of the ring. Two
          channels of one input may now bind two suffix rings, as aliased tile
          channels already bind two representations.
        - *Each source's own window.* `derived_source_window()` gives each source
          view of a `materialized` or `composed` input its own window:
          - its in-flight count is its read delay minus its latency. The new
            `SampleRealtimePortStateStorageView::producer_latency` comes from the
            connection timings' `source_latency`; a feedback timeline counts
            latency 0;
          - its values resume as in `input_resume_position()`, applied to that
            source;
          - it inherits `[-kept history, min(c_old, T))`;
          - without a source's latency, the input's whole window applies as
            before.
          - `build_steady_storage_transfers()` uses it for every paired-source and
            fresh-only transfer of a converted or composed input.
            `sample_source_views_pair()` now pairs the same producer's channel at
            any read delay, as a `direct` input already did.
          - Still open: a converted or composed input that moves to transition-only
            storage keeps one window for the composed value. When its sources
            resume at different positions, that window is wrong; this happens
            when a rewire or a conflict in shared storage moves it.
        - *Option A.*
          - `ProjectGraphGraphJitRequest::initial_generation` marks the first
            generation; `ProjectGraph` sets it when it has no accepted execution
            graph. `GraphJit::handle_project_graph_transaction()` then calls the
            new `compile_initial_generation()`.
          - That compiles a cutover from the empty graph, with empty predecessor
            requirements and realizations.
          - The result keeps the cutover, marked
            `CompiledGraphCutover::from_empty_graph`, only when NodeStorage
            initialization does not already give every window its fresh value:
            some state needs transition-only storage, or a fresh-only transfer
            writes a value other than its region's declared initial value
            (zero bytes when it declares none).
          - `GraphExecutor::stage()` accepts such a cutover only for a first
            generation. `initialize_first_generation()` prepares and runs its
            transfers right after `NodeStorage::initialize()`, on the staging
            thread. It then records each expiry cursor's activation index and
            starts the generation in its transition phase when it has one.
          - `compile()` and a request without `initial_generation`, such as the
            manual-control recompile, still produce the steady artifact.
        - *Changed test entry point.* `CompensatedInputBesideHistoryReaderReadsItsNeutralValueBeforeItsSourceStarts`
          compiled through `compile()` and ran `NodeStorage` directly, a path that
          option A leaves steady-only. It now compiles the first-generation
          request and runs its cutover as `initialize_first_generation()` does.
          Its expectations are unchanged.
        - *Rename.* `TransitionCursorProgress::activation_ordinal`
          (graph_executor.h) is now `activation_index`: the Sequential cursor's
          value at activation.
        - *Copy cost.* Separate suffix rings made each ring's mirror copy every
          channel of its source representation, doubling the transition root's
          copying for a stereo input whose channels separate.
          `SampleMaterializationPlan::transition_channels` now lists the channels
          a suffix ring holds, and `emit_sample_materialization()` stores only
          those. The total stays one copy per moved channel.
      - No reproduction test yet for an anonymous node's raw regions. Six tests already
        assert that an anonymous node's compiler-owned regions migrate, and that
        contradicts the human co-author's statement that the state of a node
        under no virtual node is never preserved:
        `UnequalLatencySampleFeedbackMigratesAlignmentPrehistory`,
        `UnequalLatencySampleFeedbackMigratesPopulatedAlignmentRing`,
        `MultipleSampleDetachBranchesMigrateSharedHomeAndFallbackCopy`,
        `FeedForwardEventFanInCompactCarryMigrates`,
        `ExactTypeEventDetachFeedback` and `ExactSampleDetachFeedback`. Their
        fixtures call `configure_package_definition_provider()` directly, which
        skips `consume_builder_node_source_identity()`, so their nodes join no
        virtual node. *(Confirmed 2026-11-09: it covers compiler-owned storage,
        except a detach's structures; see "Decisions" above. The six tests will
        need nodes with identities.)*
    - *Known gaps, not implemented:*
      - A fresh position in a Sequential input's window is zero-filled
        (`execute_realtime_port_state_transfers()`). A freshly initialized
        feedback timeline holds its `detach()` initial value, which defaults to
        the input's `neutral_value`. *(Addressed by the fixes above.)*
      - When a composition runs before its consumer (`before_execution_position`),
        `specialize_transition_plan()` appends the transition-only input's
        `SampleMaterializationPlan` after that consumer's callback. Only routed
        compositions run before their consumer. *(Addressed by the fixes above.)*
      - A converted or composed input's per-source transfers give each source's
        storage its own fresh value. An input that sums two ordinary producers
        with coefficients 1 and 1 therefore reads twice its neutral value at a
        fresh position. *(Not reachable, 2026-11-08: the registry declares only
        mono and stereo. Mono to stereo uses 1, stereo to mono uses 0.5 and 0.5,
        so every conversion, and every composition of conversions, sums to 1
        per target channel. A realtime composition rejects two contributions to
        one target channel, and the DSL has no fan-in. Per-source fresh values
        therefore compose to the input's neutral value.)*
      - A freshly started generation does not follow the fresh-value rule. An
        input with a non-zero `neutral_value` that reads an ordinary producer's
        storage before that producer's first value reads zero bytes.
        *(Addressed 2026-11-09 where every state with a window in a region agrees
        on its fresh value: `plan_fresh_sample_storage_values()`. Still open
        where they disagree; see the last gap below.)*
      - A moved (transition-only) input whose same producer's latency grew needs
        the 8-byte resume index; lowering reports an error for it instead.
        *(Test: `GrowingLatencyOfRetainedSourceExposesNeutralValues`.
        Addressed 2026-11-09 by the cutover index cell.)*
      - A moved converted or composed input keeps the mirror at its read delay,
        so after a rewire it does not play its in-flight values first. (The
        "mirror" is the `SampleMaterializationPlan` that
        `specialize_transition_plan()` adds to copy the input's steady
        representation into its transition-only suffix ring.) *(Applies to
        `materialized` and `composed` records, not to aliased channels, which
        are `direct`. Tests: `RewiredDownmixedInputReadsItsInFlightValuesFirst`
        and `RewiredDownmixedTileReadsItsInFlightValuesFirst`. Addressed
        2026-11-09 by the cutover index cell.)*
      - A converted or composed input whose window inherits nothing stays
        `fresh_state`, so its fresh positions are not zeroed in shared storage.
        *(Tests: `GrowingCompensationOfDownmixedInputExposesNeutralValues`
        and `GrowingCompensationOfDownmixedTileExposesNeutralValues`. Addressed
        2026-11-09 by `fresh_window_realization()`.)*
      - An input that was disconnected, or only slot-fed, has no predecessor
        record, so when it becomes connected it reads the new source's values from
        before the cutover. *(Also: its fresh claim moves the other readers of
        that storage to transition-only storage instead of itself. Addressed
        2026-11-09 by `make_fresh_input_transition()`.)*
      - `input_resume_position()` recognizes the same producer only between two
        `direct` records, so a `callback_only` predecessor gets `T = c_new`. Its
        input loses the fresh positions that its producer's grown latency
        exposes. (Added 2026-11-09. Test:
        `GrowingSourceLatencyExposesNeutralValues`. Addressed 2026-11-09 by
        `input_single_producer()`.)
      - Existing tests that count records or expect a transition root that only the
        old shared-target rule produced will need new expectations. *(One found
        and changed, 2026-11-09: see "Changed test" above.)*
      - Compiler-owned raw-region identities name a node by its bundle handle,
        so renumbering bundles restarts the region. *(Test:
        `UnequalLatencyFeedbackUnaffectedByAddedLeadingNode`. Addressed
        2026-11-09 for nodes with a stable identity.)* An anonymous node's
        regions still use its bundle handle. That contradicts
        `StableConcreteNodeId`, which deliberately gives an anonymous node no
        identity. Giving its regions an empty identity would restart them at
        every edit. `UnequalLatencySampleFeedbackMigratesPopulatedAlignmentRing`,
        whose builder-made nodes are anonymous, expects them to migrate. This
        needs a decision. *(Decided and implemented 2026-11-09; see "Identity
        rule".)*
      - A composed input whose sources are different producers gets
        `T = c_new` even when one source's latency changed. Its sources'
        values resume at different positions, which one window `[-history, T)`
        cannot describe. (Added 2026-11-09. Test:
        `GrowingCompensationOfComposedSourcesWithUnequalLatencies`. The
        unequal positions arise when the sources' latencies differ and the
        input's read delay changes, not only when a source's latency changes.)
      - Channels of one port that share a suffix ring share one mirror, so they
        must agree on its delay and now also on its first frame. Lowering reports
        an error otherwise, as it already did for the delay. (Added 2026-11-09.
        Test: `AddedFeedbackChannelReadsItsDetachValuesBesideItsContinuingChannel`.)
      - *For discussion (2026-11-09): a fresh start where two states need
        different fresh values in one region.*
        - Example: an input with `neutral_value` 100 and compensation 7 and a
          history reader with neutral value 0, both reading one ramp.
        - One of them needs transition-only storage for its first positions,
          which means the first generation needs a transition root.
        - Today a first generation carries no `CompiledGraphCutover`.
          `GraphExecutor::stage()` rejects one with a cutover.
          `compatible_realtime_code()` rejects a steady-code replacement with a
          cutover, and `ProjectGraph`'s manual-control recompiles stage
          `compile()` results as such replacements.
        - So the first generation's transition root would need its own artifact
          contract, separate from `compile()`'s steady artifacts. That is not a
          cheap fix.
        - Until it is decided, `plan_fresh_sample_storage_values()` leaves such
          a region at zero bytes, as before, and those states read zero.
          *(Test: `CompensatedInputBesideHistoryReaderReadsItsNeutralValueBeforeItsSourceStarts`.)*
        - `ProjectGraph`'s manual-control recompile sends the same request as a
          first generation (`ProjectGraphGraphJitRequest` without a
          predecessor), so `GraphJit` cannot tell the two apart.
        - Observed while checking this, not confirmed by a run:
          `compatible_realtime_code()` also requires an equal `storage_size` and
          equal regions. So a `compile()` replacement already cannot replace the
          code of a generation compiled with transition-only storage, and
          `stage_realtime_code()` throws `std::invalid_argument` for it.
          *(Intended to work; test:
          `CompatibleCodeReplacesSteadyCodeOfGenerationWithTransitionRoot`.)*
        - *Decided 2026-11-09: option A* (see "Decision" above).
  - *Second run (repository owner, 2026-11-07): all 794 tests pass.*
    `iv_edit_latency_benchmark --shape feedback --size 64` now completes: all 26
    edits are accepted. Medians per edit kind are 573 to 587 ms of wall time, against
    the 200 ms target. Of that, `GraphJit` takes 571 to 585 ms: optimize 214 to 217
    ms and codegen 335 to 341 ms, while capture, plan, emit and verify together take
    about 20 ms. Each edit is a transition compile of one module with 3 functions,
    24,395 to 24,462 instructions before optimization and 14,471 to 14,495 after.
    The realtime tick mean is 1,958 to 1,988 µs per 5,333 µs block.
- **Active feature (2026-11-09): one lifetime and ownership model for sample and event
  streams.** Handoff record (development_workflow.md 3.5); move it to
  `docs/historical/` when the feature is complete.
  - *Decision (human co-author, 2026-11-09).* Sample and event streams are implemented
    symmetrically in the lifetime and ownership of their data. The places where they
    differ are listed explicitly; a difference outside that list must disappear.
    Events adopt the sample port time convention of 2026-11-07. The common part is
    implemented once and used by both kinds, rather than duplicated.
  - *Approach (collaborator's choice of the two the human co-author offered).* First
    extract the reusable parts of the sample implementation into utilities, without
    changing sample behavior; then rebuild the event implementation on them.
  - *Normative destination.* sequential_port_storage_planning.md, section "Sample and
    event streams share one lifetime and ownership model" and "Tick event ports need
    bounded time windows"; graph_jit_direction.md, "Port history and latency are
    node-owned semantic state"; the glossary's "Node-owned port state". The
    superseded text is in historical/event_output_future_window_2026-11-09.md. The
    allowed differences: what one position holds (one value, overwritten, or a set of
    events, added to); what a fresh position holds (detach initial value or
    `neutral_value`, or no events); how contributions combine (sum through
    coefficients, or merge by position, source index, then authoring order).
  - *Current event implementation against that destination.*
    - `EventOutputPort` (ports.h) validates against
      `realtime_port_window(block, size, history, latency)`, which is
      `[B - history, B + N + latency)`. Destination: `[B - latency, B + N)`.
    - `EventInputPort::get_block()` returns events at their own timestamps and skips
      events older than the block. Destination: events reach the input
      `read_latency` later than their source authored them, and none arrive late.
    - `plan_sample_latency_compensation()` (connection_plan.cpp) assigns
      compensation to sample connections only, and `EventSourcePlan` has no
      `read_latency`. Destination: one path-latency alignment over both kinds.
    - `inventory_realtime_port_states()` gives an event output two records
      (`tick_output_history`, `tick_output_latency`) and an event input its history
      only; `inventory_sequential_input_in_flight_values()` covers sample
      connections only. Destination: the sample record shapes.
    - `RealtimePortStateRole::tick_output_latency`,
      `EventRealtimePortStateFutureSource` and
      `EventRealtimePortStateRealization::retained_future_sources` exist only for
      the future window; the cutover seeds future events through them.
    - Event carry commit (`iv_graph_jit_commit_event_carry()`) keeps
      `[end - history, end + latency)`; event feedback appends shift by
      `loop_extra_latency` only, not by source latency.
    - Event detach structures are identified by producer group, source position
      and latency (lowering_plan.cpp), not through `resolve_detach_identities()`.
    - Event transition state has no transition-only storage:
      `realize_realtime_port_state_transition_storage()` rejects it.
  - *Milestones.*
    - M0, done: the normative destination above.
    - M1: shared utilities extracted from the sample implementation and used by it,
      with no change to sample behavior. Events still use their own code, so every
      test should still pass. Planned parts: path-latency alignment, port-state
      inventory, in-flight counts, detach identity parts, transition windows
      (`make_transition()`, `input_resume_position()`, `derived_source_window()`),
      claim-based conflict resolution, and transition-only bookkeeping (suffix
      groups, expiry cursors, cutover cells).
    - M2: events rebuilt on those utilities under the new convention: authoring
      window, read latency with compensation, delivery restamped by read latency
      (a detach's shift becomes one case of it), one output record, input
      in-flight records, event detach identities, event transition-only storage;
      removal of the future-window roles.
    - M3: tests. Tests that encode future stamping change
      (`EventFeedbackSccRetainsAuthoredFutureEvents`,
      `EventFeedbackSccPersistentRingRetainsAuthoredFutureEvents`, the
      `RetainedTriggerEventSource` tests, and other uses of
      `LatentEventFeedbackA` and `PersistentLatentEventFeedbackA`); new tests cover
      event compensation, event in-flight ownership and the event authoring window.
      `GrowingEventDetachDeliversItsInFlightEventsFirst` and
      `RewiredComposedInputReadsItsLatentSourceFinalValues` are pending runs.
  - *Expected breakage.* During M2, event lowering and event tests do not build or
    pass until the event layer is complete. Sample tests must keep passing
    throughout.
  - *Representation consequence, not friction.* An event carries its timestamp, so a
    read delay cannot be a storage offset as it is for samples: an event input with a
    nonzero read delay receives a copy of its window restamped by that delay, as a
    detach's delayed stream already is. The copy costs one event write per delivered
    event.
  - *M1 status (2026-11-09; collaborator run: 830 of 832 tests pass).* Extracted,
    each still called for samples only:
    - connection_plan.cpp: `plan_latency_compensation()` (was
      `plan_sample_latency_compensation()`) aligns `IncomingStreamPath` records
      through `align_incoming_paths()`; `append_sample_incoming_paths()` and
      `summarize_sample_read_latencies()` are the sample hooks.
      `in_flight_extent()` and `record_input_in_flight()` hold the in-flight rule
      and the input record update. `detach_identity_port()` and
      `detach_identity_settings()` (connection_plan.h) build detach identity parts;
      `SampleDetachIdentities` uses them, with unchanged identity text.
    - realtime_port_state_transition.cpp: `StreamSourceTiming` (producer port and
      channel, read delay, producer latency, detach timeline) with
      `sample_source_timing()`, `single_producer()`, `sample_input_sources()`;
      `input_resume_position()`, `derived_source_window()` (now returning
      `SourceWindow`), `apply_input_window()` and `make_fresh_input_transition()`
      take timings, a resume position and a fresh-window kind instead of sample
      realizations; `StorageClaim`, `storage_claims_agree()` and
      `settle_storage_claims()` hold claim settlement for
      `resolve_sample_transfer_conflicts()`.
    - Expected: all tests pass except the two reproduction tests still pending
      (`RewiredComposedInputReadsItsLatentSourceFinalValues`,
      `GrowingEventDetachDeliversItsInFlightEventsFirst`). Observed in that run:
      exactly those two fail, as predicted.
      - `RewiredComposedInputReadsItsLatentSourceFinalValues`: only position 1030
        (`C + 6`) differs; it reads 1023 instead of 1073, the downmix of the
        revising source's final value 1123 and the ramp's 1023.
      - `GrowingEventDetachDeliversItsInFlightEventsFirst`: the slice at 64
        receives no trigger instead of the in-flight trigger at 67.
    - Transition-only bookkeeping (suffix groups, expiry, cutover cells) is not
      extracted yet. `realize_realtime_port_state_transition_storage()` already has
      a separate event suffix path (`EventSuffixTargetGroup`) that differs from the
      sample one; the shared shape is decided when M2 rebuilds the event path.
    - `same_producer_port()` ignores the producer channel, as before. That is
      correct: every channel of one Tick output shares its latency and so its
      finalization frontier, which is what the resume position depends on.
  - *M2 representation decision (collaborator, 2026-11-09; a storage choice, no
    observable effect).* A latent Tick event output's retained streams hold each event
    at the position where it becomes final, its authored position plus the output's
    latency. The callback still authors at the event's own position, in
    `[B - latency, B + N)`; the merge from the callback sequence into retained
    storage adds the latency. Reasons: an event carries its timestamp, so every read
    delay is a restamping copy; a latent output already needs that merge, because a
    new event may land among events it authored earlier; and in that frame every
    consumer whose read delay is the source latency (no compensation) reads the
    stream without a copy, and the existing group, carry, ring and detach pipeline
    keeps its time frame. The M0 sentence "Storage files each event at the position
    it was authored for", written earlier the same day, was removed from
    sequential_port_storage_planning.md, graph_jit_direction.md and the glossary,
    since the same section leaves representations free.
  - *M2 status (2026-11-09; not yet built).*
    - Connection analysis (connection_plan.h/.cpp):
      `EventDeliveryPlan::read_latency`; `append_event_incoming_paths()` feeds
      event deliveries to `plan_latency_compensation()`; event input records get
      in-flight counts through `record_input_in_flight()`; an event output has one
      `tick_output_history` record of `history + latency`;
      `RealtimePortStateRole::tick_output_latency` is gone; event group retention
      adds each target's compensation.
    - Authoring window: `tick_output_authoring_window()` (ports.h);
      `EventOutputPort` validates `[B - latency, B + N)`.
    - Restamping: `EventMergePlan::source_delays`, emitted as a trailing `delay`
      argument of `iv_graph_jit_merge_event_sequence()` and
      `iv_graph_jit_merge_ordered_event_sequence()`. The raw output merge, both
      fan-in merges and the single-source group merge delay each source by its
      latency.
    - Compensation: first a per-target delayed materialization
      (`EventMaterializationPlan::delay_samples`); replaced by compensation in
      producer-group streams (see "Fixes" after the third M2 run).
    - Detach: a connection detach's feedback append reads the group stream through
      `EventFeedbackPlan::selects_final_window`: each slice copies the events that
      became final in it, `[index, index + block_size)`, so an event added among
      earlier ones is still delivered once (the four feedback helpers take
      `select_final_window`, `window_begin`, `window_extent`). A per-source detach
      reads the callback sequence and delays it by the source latency plus the
      detach latency.
    - Realizations: `EventRealtimePortStateStorageView::timeline_offset` (output
      views `+latency`, compensated input views `-compensation`, detach streams and
      transition suffixes 0); `EventRealtimePortStateSourceTiming` per delivery;
      `EventRealtimePortStateFutureSource` and `retained_future_sources` removed.
    - Transitions: event input transitions use `apply_input_window()` with
      `event_input_sources()`; event transfers carry
      `source_timeline_offset`/`target_timeline_offset`, select by the source offset
      and restamp by the difference; the future-source seeding is removed.
  - *Still open in M2 (not implemented).*
    - Event claims and fresh windows: `resolve_sample_transfer_conflicts()` has no
      event counterpart, and an event input that the predecessor lacks gets no
      `make_fresh_input_transition()`. A new consumer of shared event storage can
      therefore see the producer's old events in its window, against "do not
      fabricate old consumer history". *(Resolved 2026-11-10: event fresh windows
      and claims.)*
    - An event transition-only input suffix is mirrored from its steady binding
      from the cutover on without skipping the positions its inherited in-flight
      window already holds (no cutover cell for events). *(Open; see "Remaining"
      below.)*
    - A fan-in input whose sources need different compensation is rejected by
      `target_compensation()` with an explicit error. *(Resolved 2026-11-10:
      compensation in producer-group streams and per-target partitions.)*
    - Persisted capture of a latent event output (`tick_invocation_frame.cpp`)
      records the callback window `[B, B + N)`, not the positions that became final.
      *(Resolved 2026-11-10: a record covers what its block finalizes.)*
    - Event detach structures are still identified by producer group, source and
      latency, not through `resolve_detach_identities()`. The in-flight events now
      also transfer through the input's record. *(Open, cleanup; see "Remaining".)*
    - The feedback cursor plumbing (`initialize_event_feedback_cursors_after_carry_restore()`
      and the cursor allocas) is no longer read by any feedback append.
      *(Removed 2026-11-10.)*
    - The compatibility runtime's `EventInputPort` (ports.h) reads events at their
      own timestamps. *(Withdrawn 2026-11-10: it is the facade GraphJit callbacks
      use, and it only selects a window from storage that already holds events at
      the input's positions.)*
  - *Expected test changes (M3).* Tests whose nodes author after their block or
    expect events at their authored timestamps through a latent output now fail,
    for example the `LatentEventFeedbackA`, `PersistentLatentEventFeedbackA` and
    `RetainedTriggerEventSource` tests.
  - *First M2 run (collaborator build and test run, 2026-11-09): 824 of 832 tests
    pass.* `GrowingEventDetachDeliversItsInFlightEventsFirst` now passes: its
    in-flight trigger transfers through B's input record.
    `RewiredComposedInputReadsItsLatentSourceFinalValues` still fails (sample gap,
    unchanged). The other seven failures:
    - Five tests threw "realtime event output timestamp is outside the current
      latency + block window": their nodes authored after their block
      (`RetainedTriggerEventSource`, `LatentEventFeedbackA`,
      `PersistentLatentEventFeedbackA`, `PersistentLatentBoundaryEventFeedbackA`).
    - `InventoriesStableNodeOwnedRealtimePortState` counted the removed event
      latency record.
    - `SeparateEventOutputAndInputStateNeedsNoTransitionRoot` found 2 event
      transfers instead of 3. That exposed a gap: a producer group's own stream
      also holds its sources' latency positions, which belong to the outputs, and
      M2 had dropped the seeding that copied them into the successor's stream.
  - *Fix after the first M2 run.* `EventRealtimePortStateOutputSource` and
    `EventRealtimePortStateRealization::retained_output_sources` name, for an input
    whose retained storage is its producer group's own stream, each latent source
    output. `build_steady_storage_transfers()` seeds each output's latency positions
    `[-latency, 0)` into that stream once, as a supplemental transfer with target
    offset `+latency`, unless the stream is the output's own storage.
  - *M3 test updates after the first M2 run.*
    - `RetainedTriggerEventSource` now authors `B - 5` (inside its latency window,
      before the event it authored at the previous block's end - 3), `B + 5` and
      `B + N - 3`. `CompactRetainedEvents`,
      `SeparateEventOutputAndInputStateNeedsNoTransitionRoot` and
      `FeedForwardEventFanInCompactCarryMigrates` expect the consumer to read them
      `latency` later, in position order (67 before 69 in the second call).
    - `LatentEventFeedbackA`, `PersistentLatentEventFeedbackA` and
      `PersistentLatentBoundaryEventFeedbackA` author `latency` positions earlier
      than before, so consumers that read at the output's latency see the same
      positions. A detach now copies each event when it becomes final rather than
      when it is authored, so each feedback ring or carry holds one event fewer at
      a root-call boundary: the expected carry counts and ring write indices drop
      by one (`EventFeedbackSccRetainsAuthoredFutureEvents`,
      `EventFeedbackSccPersistentRingRetainsAuthoredFutureEvents`,
      `EventFeedbackSccComposesLatencyHistoryAndConversion`).
    - `InventoriesStableNodeOwnedRealtimePortState` expects 6 records and one
      event output record of 24.
  - *Second M2 run (collaborator, 2026-11-09): 831 of 832 tests pass.* Only
    `RewiredComposedInputReadsItsLatentSourceFinalValues` fails, as before (the
    sample gap). Package part 2 first exceeded its 61,440-byte limit by 16 bytes;
    the new test-node comments were shortened.
  - *Open items reassessed against observable behavior (2026-11-09).*
    - The event suffix's missing cutover skip is not observable today. When an
      event state moves to transition-only storage, `specialize_transition_plan()`
      clears the migration identity of its steady stream, so the steady stream
      holds no pre-cutover event at the input's in-flight positions; the only
      events it receives before the mirror reads it are the seeded latency
      positions of its sources, which lie after the window.
    - An added consumer that reads a shared stream directly counts as a second
      user in `classify_shared_successor_storage()`, which moves the surviving
      state to transition-only storage and clears the stream. One that reads it
      through a materialization (conversion or compensation) is not counted, so
      the surviving state's transfer fills the stream it reads.
  - *Reproduction tests (2026-11-09; not yet run).* Their modules are at the end of
    package part 4, now 59,682 of 61,440 bytes.
    - `AddedEventConsumerReadsNoEventsFromBeforeItsCutover`.
      - `added_event_consumer_module(added)`: `MidiEventSource` (note-ons at 5 and
        17, a note-off at 9, every block) read by `RetainedMidiEventConsumer`
        (history 160), and with `added` also by `PersistentEventFeedbackConsumer`
        (trigger, history 320) through the MIDI-to-trigger conversion.
      - Expects the survivor to keep its history (6 events in `[-96, 128)`) and
        the added consumer to read only 69 and 81.
      - Expected to fail with 4 events, first 5: the survivor's transfer fills the
        group stream, and the added consumer's materialization reads its
        pre-cutover note-ons.
    - `EventFanInAlignsSourcesOfUnequalLatency`.
      - `configured_unequal_latency_event_fan_in_graph()`: `RetainedTriggerEventSource`
        (latency 8) and `TriggerEventSource` (latency 0) fan in to
        `RetainedTriggerEventConsumer` (history 8). Both deliveries have read
        latency 8.
      - Expects the consumer to read both sources 8 positions late: 3, 11, 13, then
        67, 69, 71, 75, 77.
      - Expected to fail compiling, with "GraphJit event fan-in whose sources need
        different latency compensation is not implemented"
        (`target_compensation()`).
  - *Third M2 run (collaborator, 2026-11-09): 831 of 834 tests pass.* Both
    reproduction tests fail as predicted: the added consumer reads 4 events, first
    5; the fan-in fails to compile with the "different latency compensation" error.
    `RewiredComposedInputReadsItsLatentSourceFinalValues` fails as before.
  - *Fixes (2026-11-09; collaborator build and test run pending).*
    - *Event fresh windows and claims.* `event_fresh_window_realization()`: an event
      input the predecessor lacks gets `make_fresh_input_transition()`, and one
      whose window inherits nothing gets that kind from `apply_input_window()`.
      Either claims its positions in the stream it reads with a fresh-only
      transfer (`source_access == immutable_empty`, empty inherited range), which
      leaves them without events. `resolve_event_transfer_conflicts()` builds a
      `StorageClaim` per event transfer, keyed by target stream and source index,
      keeps seeding transfers and an output's own unchanged-latency transfer as
      native, and settles each stream with `settle_storage_claims()`, the same
      code the sample conflicts use. A moved state's seeding transfers stay. A
      fresh transition-only event input gets an empty suffix. Validation and
      execution accept a fresh-only transfer, which reads nothing. A fan-in stream
      with a source-index sidecar that the input does not select from is not
      claimed (open).
    - *Compensation in producer-group streams.*
      `EventProducerGroupPlan::source_compensations` keys each group by its
      readers' per-source compensation, so a group's stream holds each source's
      events where its readers read them: every merge into it delays a source by
      its latency plus its compensation, its retention counts the read latency
      ahead of the block, the raw output stream serves a single-source group
      directly only without compensation, and the group's raw identity names its
      compensation. An input's view of its group stream therefore has offset 0,
      and the seeding of an output's latency positions uses
      `EventRealtimePortStateOutputSource::stream_delay`. This replaces the
      per-target delayed materialization of the first M2 pass:
      `target_compensation()`, `EventMaterializationPlan::delay_samples` and the
      materialization helper's `delay` argument are removed. Detach lookups find
      a connection's group by membership, since groups of the same sources may
      differ in compensation. A connection whose targets need different
      compensation for one source is rejected with an explicit error (open).
  - *Fourth M2 run (collaborator, 2026-11-10): 832 of 834 tests pass.*
    `EventFanInAlignsSourcesOfUnequalLatency` passes.
    `AddedEventConsumerReadsNoEventsFromBeforeItsCutover` failed only in
    `run_across_graph_edit()`: the surviving consumer's 160-position transition
    root had not expired by 128. The test now runs to 256 and also checks the
    calls after the transition root.
  - *Open items without a reproduction test (2026-11-10).*
    - Event detach identities: not observable. The input's record now carries a
      detach's in-flight events, and an anonymous or new consumer's fresh claim
      clears the stream, so raw migration of the delayed stream is only a
      fallback. Cleanup.
    - The feedback cursor plumbing is dead code. Cleanup.
    - The "compatibility runtime" item is not a gap: `EventInputPort` is the facade
      GraphJit's callbacks use, and it only selects a window from storage that
      already holds events at the input's positions. Withdrawn.
    - Persisted capture of a latent output. *Decision (human co-author,
      2026-11-10, after the collaborator confirmed that Tick/persisted outputs may
      declare latency):* block `B`'s record covers what that block finalizes,
      `[B - latency, B + N - latency)`, with its disposition, for sample and event
      outputs alike. coverage_and_background_evaluation.md is updated; the
      superseded paragraph is in
      historical/persisted_recording_callback_block_2026-11-10.md. Today
      `TickInvocationFrame` records `[B, B + N)` of the callback storage for both
      kinds.
  - *Reproduction and coverage tests (2026-11-10; not yet run).* New package code
    is in a fifth package part (`part_5`, 4.2 KB), since part 4 was full.
    - `EventInputIsCompensatedForItsNodesLatentSampleInput` (coverage):
      `CompensatedTriggerProbe` (sample and trigger inputs) reads a ramp of latency
      7 and `TriggerEventSource`. Expects read latency 7 and triggers at 10, then
      70 and 74. Expected to pass.
    - `RoutedEventConnectionCompensatesEachTargetOnItsOwn`:
      `configured_routed_unequal_compensation_event_graph()` drives a subgraph's
      event input that feeds the probe (compensation 7) and
      `RetainedTriggerEventConsumer` (none), one authored connection with two
      targets. Expected to fail compiling with "GraphJit event connection whose
      targets need different latency compensation for one source is not
      implemented".
    - `AddedConsumerOfIndexedEventFanInReadsNoEventsFromBeforeItsCutover`:
      `configured_staged_event_fan_in_graph(observed)` adds an acyclic
      `PersistentEventFeedbackConsumer` (history 320) reading the staged fan-in,
      whose stream has a source-index sidecar. Expects 10 events after the cutover,
      66 to 127. Expected to fail, either in the cutover's preparation ("event
      transition target has inconsistent source identity": the surviving loop
      reader's history transfer into the indexed stream names no source index) or
      with 12 events, first 58 (the added consumer's fresh window is not claimed).
    - `PersistedLatentTickOutputsRecordWhatEachBlockFinalizes`:
      `RevisingCaptureSource` (persisted sample and trigger outputs, latency 1)
      writes each sample as its position and revises it during the next position
      to 1000 plus its position, and adds a trigger at each block's last position
      and one at the position before the block. The test drives the generated root
      through a real `TickInvocationFrame` for blocks at 0 and 64 and decodes the
      queued records (`captured_records()`). Expects sample records `[-1, 63)` and
      `[63, 127)` holding revised values (999 ... 1062, 1063 ... 1126), and event
      records with the event at -1, then the two events at 63. Expected to fail:
      the records begin at 0 and 64, the samples hold 1000 ... 1062 and the
      unrevised 63, and the event records hold 63 and 127.
  - *Fifth M2 run (collaborator, 2026-11-10): 834 of 838 tests pass.*
    - `PersistedLatentTickOutputsRecordWhatEachBlockFinalizes` failed as predicted
      (records begin at 0 and 64, unrevised last values, events at 63 and 127).
    - `AddedConsumerOfIndexedEventFanInReadsNoEventsFromBeforeItsCutover` failed
      in the cutover's preparation: "event transition target has inconsistent
      source identity".
    - `RoutedEventConnectionCompensatesEachTargetOnItsOwn` failed for a test
      reason: its graph builder passed the ramp's latency argument without a
      registered type. Its graph is now `routed_unequal_compensation_module`.
    - `RewiredComposedInputReadsItsLatentSourceFinalValues` failed as before.
  - *New reproduction test (2026-11-10; not yet run).*
    `RecompiledEventFanInOfUnequalLatencyReadsEachEventOnce`: the unequal-latency
    fan-in with identified nodes, recompiled across a cutover at 64. Expects the
    steady reads (67, 69, 71, 75, 77; then 131 ... 141). Expected failure before
    the fixes below: 69 twice. The input's one in-flight window `[-8, 8)` also
    covers the latent source's latency positions, which are seeded as well; the
    two claims disagree, the input moves to transition-only storage, and its
    mirror copies the seeded 69 again (the mirror-skip gap, observable here).
  - *Fixes (2026-11-10; collaborator build and test run pending).*
    - *Per-target compensation in one connection.* `plan_event_groups()` partitions
      a connection's realtime targets by their per-source compensation; the
      connection joins one group per partition, and
      `EventProducerGroupPlan::connection_targets` (parallel to
      `connection_indices`) names the targets that read it. Lowering binds
      `group_targets(group, connection_index)` instead of every realtime target.
    - *Persisted capture records what each block finalizes.* Sample capture shifts
      its recorded range and read index back by the output's latency. Event capture
      keeps `EventCaptureSlot::pending_events` (sized off the audio thread from the
      output's rate and latency): each block records the pending events it
      finalizes and its own events in `[B - L, B + N - L)`, earlier-authored first
      at an equal position, and keeps the rest pending; a voided block erases its
      window; an untouched block with pending events publishes just those
      positions. The `TickInvocationWorkspace` capture tests in
      persisted_data_store_gtest.cpp used latency 1 with expectations of the old
      rule; their plans and bindings now use latency 0, since they test capture
      mechanics.
    - *Whole indexed fan-in streams.* An input reading a whole stream with a
      source-index sidecar transfers each event with its sidecar index from the
      predecessor (`copies_source_indices` in execution); without a predecessor
      sidecar it moves to transition-only storage. Its fresh window claims the
      stream too.
    - *One window per source in a fan-in.* A group whose sources need different
      compensation keeps a source-index sidecar (`requires_source_identity`). An
      input reading such a whole stream transfers one window per source, from
      `derived_source_window()` with `EventRealtimePortStateSourceTiming::source_index`;
      a steady event transition may now have several transfers.
  - *Still open (2026-11-10).*
    - `RewiredComposedInputReadsItsLatentSourceFinalValues`: a composed sample
      input moved to transition-only storage keeps one suffix for the composed
      value, and its mirror copies the steady composition, which reads every
      source live. Where one source still plays its in-flight value and another
      has resumed, neither can hold the mix. Planned fix: the transition root
      points each composition source at its own suffix ring (window and mirror per
      source, the direct-input suffix code with its cutover cell).
    - The event mirror skip: an event input moved to transition-only storage still
      mirrors its steady stream from the cutover on. With per-source windows, no
      known graph reaches it; not implemented.
  - *Sixth M2 run (collaborator, 2026-11-10): 837 of 839 tests pass.* All new event
    tests pass except `AddedConsumerOfIndexedEventFanInReadsNoEventsFromBeforeItsCutover`
    (12 events, first 58): the surviving loop reader's per-source claims were keyed
    by source index and its whole-stream fresh competitor by none, so they were
    never compared. Fix: `resolve_event_transfer_conflicts()` keys claims by stream
    only, and `StorageClaim::lane` (the source index a claim fills) makes claims on
    different sources of one stream disjoint.
  - *Composed-input design (2026-11-10; implemented, collaborator build and test run
    pending).* For a `composed` sample record
    whose sources do not all pair, only the unpaired sources need transition-only
    storage:
    - paired sources (same producer, `sample_source_views_pair()`) keep their
      steady per-source transfers (`derived_source_window()`);
    - each unpaired source gets a source suffix ring that stands in for the current
      source view's storage (same geometry and timeline offset): the predecessor
      source's window by `derived_source_window()`, then a mirror from the new
      source's ring from that window's end, as a `direct` input's suffix is filled;
    - the transition root's `SampleCompositionPlan` reads that source from its
      suffix (the `SampleCompositionInputPlan` is re-pointed, read latency
      unchanged). No new emission features are needed.
    - Implementation: `sample_sources_partially_pair()` makes such a record
      `transfer_to_steady_storage`; `build_steady_storage_transfers()` gives each
      rewired source a transfer with `SampleRealtimePortStateTransfer::composition_source`
      and `target_transition_only`; `realize_realtime_port_state_transition_storage()`
      allocates its ring (a compact carry expands to its working ring) and a cutover
      cell; validation accepts a source suffix of a steady state;
      `specialize_transition_plan()` re-points the matching
      `SampleCompositionInputPlan` (matched by channel, read latency and retained
      NodeStorage offset), adds the mirror after the new source's step, and points
      the transition realization's composition source at the suffix. Any source
      suffix makes the cutover need a transition root.
  - *Seventh M2 run (collaborator, 2026-11-10): all 839 tests pass.*
  - *Reproduction tests (2026-11-10; not yet run).* Package part 5 is now 9.4 KB.
    - `LatentEventOutputReadDirectlyKeepsItsUnfinalEventsAcrossARecompile` and
      `...AcrossALatencyChange`: `latent_direct_event_module(latency)`:
      `LatentTriggerSource` (history 8, the given latency; authors `B - 1`, `B + 5`
      and `B + N - 2`) read by `RetainedTriggerEventConsumer` (history 8), which
      reads the output's own retained stream. Recompiled with latency 4, then
      changed from 4 to 6, at 64. Expects the output's event authored at 62, not
      final at the cutover, read at 66 (resp. 68 after 2 fresh positions).
      Expected failure: `classify_shared_successor_storage()` sees two direct
      users of the output's own stream, moves both states to transition-only
      storage and clears the stream; the seeding skips the output's own storage,
      so nothing restores the unfinal event (66 resp. 68 missing; counts 2).
    - `PersistedLatentPacedSampleOutputRecordsWhatEachBlockFinalizes`:
      `RevisingPacedCaptureSource` (persisted samples at pace 2, latency 1 value)
      through a real `TickInvocationFrame`. Expects each record one value earlier
      than the block's lattice range (computed with `try_project_onto_lattice()`),
      holding revised values. Expected to pass if latency counts values of the
      output's pace, as the capture assumes; a failure would show otherwise.
  - *Cleanup (2026-11-10).*
    - The feedback cursor plumbing is removed from lowering.cpp: the cursor
      allocas, `initialize_event_feedback_cursors_after_carry_restore()`, and the
      cursor parameter of `emit_event_feedback_append()` and the event operation
      emitters. graph_jit_direction.md's status text (point 12, and the
      lifecycle paragraph) now describes the final-window append.
    - `EventFeedbackSccRetainsAuthoredFutureEvents` and
      `EventFeedbackSccPersistentRingRetainsAuthoredFutureEvents` are renamed
      `...RetainsUnfinalLatencyEvents`; the run records above keep the old names.
    - Stale comments refreshed: `realize_realtime_port_state_transition_storage()`
      and `plan_fresh_sample_storage_values()` (realtime_port_state_transition.h),
      `inventory_sequential_input_in_flight_values()` (connection_plan.cpp).
  - *Eighth M2 run (collaborator, 2026-11-10): 840 of 842 tests pass.* Both
    `LatentEventOutputReadDirectly...` tests fail as predicted (counts 2; 66 resp.
    68 missing). `PersistedLatentPacedSampleOutputRecordsWhatEachBlockFinalizes`
    passes: latency counts values of the output's pace.
  - *Fix (2026-11-10; collaborator build and test run pending).* A Tick output's
    storage belongs to its producer port:
    - `classify_shared_successor_storage()` no longer moves an output's own
      transition when several direct users share its stream; only the readers
      move to transition-only storage;
    - `resolve_event_transfer_conflicts()` counts every event output transfer into
      its own storage as native, whatever restamp (a latency change) or window
      growth it has, so a reader that disagrees moves instead.
    - Sample outputs keep their narrower native rule (same producer, no shift, no
      fresh positions), since there a grown output window and a reader's native
      claim could both be native and disagree. Whether a sample output whose window
      grows can lose its values to a larger reader's claim is unchecked.
  - *Ninth M2 run (collaborator, 2026-11-10): all 842 tests pass.*
  - *Sample counterpart (2026-11-10; test not yet run).*
    `GrowingLatentSampleOutputBesideItsReaderKeepsItsRevisableValues`:
    `latent_window_growth_module(history)` (`LatentWindowHistorySource`, latency 2,
    read by `LatentWindowConsumer`, history 4), history grown from 3 to 6 at 64.
    The reader's window covers one of the output's newly exposed positions with an
    old value; its claim is native and the grown output's is not, so the output
    moves to transition-only storage. Expects every value the reader reads, in
    five blocks, to be its position's final value. Predicted to pass: a sample
    output in transition-only storage mirrors its suffix, latency positions
    included, back into its steady ring every block (`retained_before = latency`),
    which the event side lacked.
  - *Tenth M2 run (collaborator, 2026-11-10): all 843 tests pass.*
    `GrowingLatentSampleOutputBesideItsReaderKeepsItsRevisableValues` passes as
    predicted: no sample counterpart of the event output-own-storage gap.
  - *Remaining (2026-11-10).* The feature's normative destination is implemented for
    every case the tests reach. These items remain:
    - *Latency compensation and feedback.* `plan_latency_compensation()`
      (connection_plan.cpp) aligns nothing inside a cyclic region, and
      `append_sample_incoming_paths()` / `append_event_incoming_paths()` skip every
      connection with a detached source. The two parts differ:
      - *Detached (feedback) inputs: a logical consequence, not a gap.* An SCC
        exists only because one of its edges carries a `detach()` delay, and a
        feedback input's path latency is the node's own output path latency plus
        the loop's latency. Aligning it with the node's other inputs would need a
        negative delay, so its read delay is the authored one. Once the loop's
        forward edges are aligned, the loop shares one timeline and a feedback
        input reads it consistently.
      - *Non-detached edges inside an SCC, and an SCC's output path latency:
        deferred, fixable.* The code comment says "Feedback-aware SCC latency is
        deliberately deferred": a node inside an SCC whose inputs have unequal
        path latency reads them unaligned, and an SCC node reports only its
        internal latency as its output path latency, so nodes downstream of an SCC
        are compensated against an underestimate. The SCC's non-detached edges are
        acyclic (`SccRegionPlan::execution_order` is a topological order without
        detached edges), so the same per-node alignment can run over it, with
        detached inputs left at their authored delay. Compensation only adds delay
        on forward edges, so the slice quantum (bounded by detach latencies) and
        causality are unaffected; in-SCC streams must retain the added
        compensation (sample feedback timelines, event SCC aggregates and their
        merges). Both stream kinds are affected alike.
      - *Reproduction tests (2026-11-10; not yet run).* `scc_compensation_module`:
        `AlignedPairPassthrough` (compares its `ramp` and `loop` inputs at every
        position, passes `ramp` through) reads `LatencyArgumentRampSource` of
        latency 7 and `LoopRampSource`, which reads the passthrough back through
        `detach(8)`; `LatencyCompensationProbe` after the loop compares a plain
        ramp with the passthrough's output.
        - `SccNodeAlignsItsInputsOfUnequalPathLatency` expects the passthrough's
          inputs equal at all 128 positions. Expected failure: every position
          differs by 7 (the loop input is read at delay 0).
        - `NodeAfterAnSccIsCompensatedForTheSccsPathLatency` expects the probe's
          inputs equal. Expected failure: they differ by 7, since the passthrough
          reports output path latency 0, so the plain ramp is not delayed.
        - *Eleventh M2 run (collaborator, 2026-11-10): 843 of 845 tests pass.*
          Both fail as predicted: 127 of 128 positions differ by 7 (position 0
          holds 0 in both inputs).
      - *Fix (2026-11-10; collaborator build and test run pending).*
        `plan_latency_compensation()` aligns a cyclic region like an acyclic one,
        over its `execution_order`; detached inputs keep their authored delay, as
        `append_sample_incoming_paths()` and `append_event_incoming_paths()`
        already skip them, and every loop node reports its aligned output path
        latency.
      - *Twelfth M2 run (collaborator, 2026-11-10): all 845 tests pass.* Both
        tests pass; no existing feedback test changed. Resolved.
    - *Event mirror skip (unreached).* An event input moved to transition-only
      storage mirrors its steady stream from the cutover on without skipping the
      positions its inherited window already holds. It is wrong only when a kept
      transfer fills those positions in the steady stream; the steady stream's
      clearing, claim settlement and per-source windows prevent every case found.
    - *Asymmetries outside the allowlist, no known wrong output.*
      - Claim rule for an output's own storage: an event output's transfer into
        its own storage is always native; a sample output's only when the producer
        is unchanged and its window did not grow. Samples stay correct because their
        transition-only mirror writes back into the steady ring every block. Making
        the rules one needs a resolution for a grown sample output and a reader
        whose claims would both be native and disagree.
      - `select_event_inherited_state_realization()` sends every `materialized`
        (converted) event input to transition-only storage, while a converted
        sample input can transfer per source into steady storage. This costs
        transition roots, not correctness.
    - *Cleanup.*
      - Event detach structures take their raw identity from producer group, source
        and latency, not from `resolve_detach_identities()`; only raw migration,
        a fallback, uses it.
      - `classify_shared_successor_storage()` still moves every reader of a stream
        with several direct users; claim settlement now covers most of what it
        protects. Removing it changes storage behavior and needs tests.
      - `EventOutputPort::_history` (ports.h) is unused since the authoring window
        dropped history; `history()` exists only to keep the field used. The
        constructor argument comes from generated wrappers.
      - graph_jit_direction.md, point 11's status text, still says the carry keeps
        "absolute event timestamps"; a latent output's streams hold each event at
        its position plus latency.
      - Close this record: once the items above are resolved or assigned elsewhere,
        move it to `docs/historical/` (development_workflow.md 3.5), keeping its
        decisions and friction in Front 16.
  - *Next bottom-up connection.* One of the remaining items above.
- **Still open.** `iv_edit_latency_benchmark` still waits silently when the package
  watcher service fails. Its only failure signal is
  `iv_runtime_project_notification_event`. A notification subscriber in the
  benchmark would be entered twice by one refresh, because `PackageWatcher` raises
  both `rebuildStarted` and `rebuildFinished` in one source invocation. Whether
  `SocketRpcServer`, which subscribes to the same event in the app, meets the same
  re-entry is unverified.
- **Authored `State` migration identity (2026-11-08; collaborator build and test run
  pending).**
  - *Observed.* `NodeStorage` migrated a node's authored `State` only when the
    authored node type defined both `identity()` and `move()`. No package node defines
    `identity()`, so authored `State` restarted at every graph edit even though the
    graph already gives each concrete node a `StableConcreteNodeId`.
  - *Decision (human co-author, 2026-11-08).* Migration must not require an authored
    `identity()`. The node's identity in the graph supplies it.
  - *Implementation.*
    - `NodeLifecycleCallbacks::identity_fn` and the authored `identity()` hook are
      removed. `NodeLayout::NodeRecord::migration_identity` holds the identity, and
      `NodeLayoutBuilder::set_node_migration_identity()` assigns it.
    - `plan_declarations()` (lowering_plan.cpp) assigns each declared primitive with a
      `StableConcreteNodeId` the identity `"graphjit.node:"` followed by
      `migration_identity_fields()` of that id. Its virtual-node field already
      contains a hash of the `IV_NODE` type identity. Anonymous nodes and nodes
      declared inside another node's `declare()` receive none and start fresh.
    - `NodeStorage::initialize()`, `NodeStorage::prepare_migration_from()`, and the
      structural layout comparison in graph_executor.cpp read the field.
    - `migration_identity_fields()` (stable_graph_identity.h) replaces three copies of
      the same encoding in the activity, manual-control, and producer authored-extent
      raw-region identities. Their strings are unchanged.
    - `IV_PACKAGE_ABI_VERSION` was first raised to 7 because package declaration
      code fills `NodeLifecycleCallbacks`. *Decision (human co-author,
      2026-11-08):* every version constant is 0 and is never raised.
      `IV_PACKAGE_ABI_VERSION`, `archive_version`, `session_protocol_version` (and
      its check in vscode/client/src/projectSessionDiscovery.ts), and the node-event
      `formatVersion` are now 0. A package build signature already contains
      abi.h's text and the DSL precompiled header's identity, so changed host
      headers rebuild packages without a version change.
    - Tests: node_layout_storage.cpp assigns identities through the builder;
      `AuthoredStateMovesToTheSameNodeInTheNextGeneration` (graph_jit_gtest.cpp)
      checks that two nodes of one type each continue their own `State`.
  - *Observed.* `prepare_migration_from()` deferred `initialize()` to
    `Migration::commit()`, which `GraphExecutor::activate_pending()` runs on the audio
    thread, for every node whose identity matched a predecessor that it could not move
    from. With identities on every stable node, that would have run
    `RealFFT::initialize()` (FFTW plan creation) and the `SystemAudio*` device binding
    on the audio thread at every edit. No document stated why the deferral existed; it
    dates from commit `1bec0284`.
  - *Decision (human co-author, 2026-11-08).* Node initialization runs outside the
    audio thread whenever it can. Only work that initializes from a move, or that
    mutates or destroys the predecessor's node storage, runs between audio callbacks.
    Recorded in graph_jit_direction.md, section "Generation cutover".
  - *Implementation.* `prepare_migration_from()` no longer tracks identity
    collisions. A node that is not moved initializes during preparation whether or
    not its identity matches a predecessor. At commit, moved nodes run `move()`; owners
    of imported arrays (imports are patched at commit) and nodes that depend on a
    moved or deferred node run `initialize()`.
  - *Decision (human co-author, 2026-11-08): implicit `move()`.* A node type whose
    `State` can be moved has an implicit `move()`. A node type without `State` has
    none. An explicit `move()` replaces the implicit one and is valid without
    `State`, so a node can run code at migration without an empty `State`.
  - *Implementation.*
    - `details::has_implicit_move<Node>` (context.h) holds when the node authors no
      `move()` and `State` is move-constructible. `make_lifecycle_callbacks()` and
      `make_reflected_lifecycle_callbacks()` then set `move_fn` to
      `details::move_state_implicitly<Node>`. It destroys the successor's
      default-constructed `State`, move-constructs it from the predecessor's, and
      calls `details::move_node_storage_regions()` (layout.cpp). That function
      copies each `local_array` region of the predecessor into the successor's
      matching region and points the `State`'s `local_array` and nested-node spans
      back at the successor's regions. `can_move_from()` already guarantees that the
      two nodes' regions match one to one.
    - The moved-from predecessor is consumed: its `release()` does not run, and its
      destructor runs when its storage is destroyed.
    - Tests: `CompilerOwnedActivityStateMigratesAcrossCompatibleGenerations` now
      expects `ActivityProbe::State::tick_calls` to carry over. node_layout_storage.cpp
      checks an implicit move of `AutoMovable`, including its `local_array`, and an
      explicit `move()` on a node without `State`.
  - *Observed: `State` derived from construction arguments.* Virtual-node identity
    does not change when a construction argument changes, so an implicit move kept
    `State` computed from the predecessor's arguments. `RealFFT` and `InverseRealFFT`
    kept an FFTW plan of the old `transform_size`, which the backend rejected on the
    first tick; `SystemAudioInput` and `SystemAudioOutput` kept the old `device_id`
    binding; `UniformToPower` kept weights from the old `lambda`; `UniformNoise` kept
    its old distribution.
  - *Decision (human co-author, 2026-11-08).* Each node decides what happens when
    the contents of its `State` depend on its arguments. A moved node must function
    as well as one that was freshly initialized. Checking for a stale `State` at run
    time is redundant work once `initialize()` and `move()` are correct; a node that
    needs such a check is implemented incorrectly.
  - *Implementation.*
    - `RealFFT` and `InverseRealFFT` (builtin_packages/fftw/module.cpp) record
      `State::plan_size`. Their explicit `move()` keeps the predecessor's plan when
      its size equals `transform_size`, and otherwise destroys it and creates a plan
      for the new size. Creating that plan is the only lifecycle work these nodes do
      on the audio thread, and only when `transform_size` changes.
    - `iv_fftw_execute_forward_plan()` and `iv_fftw_execute_inverse_plan()` no longer
      take sizes or validate the plan, and return nothing to assert on. The nodes no
      longer assert that `State::plan` exists. Their spectrum-timeline locals and the
      inverse plan's spectrum cache key are renamed from `*_ordinal` to `*_index`.
    - `SystemAudioInput::State`, `SystemAudioOutput::State` (basic_nodes/system_audio.h)
      and `UniformToPower::State` (basic_nodes/noise.h) delete their move
      constructor, so these nodes have no implicit move. Their successor initializes
      before the edit activates. The system-audio `State` only names the
      `SystemAudioDevices` binding for `device_id`, and `resolve_input_binding()` and
      `resolve_output_binding()` lock `SystemAudioDevices::mutex_`, which the audio
      thread must not do. `UniformToPower::State` is computed entirely from the
      arguments and `tick()` never writes it.
    - `UniformNoise` records `State::seed`. Its explicit `move()` continues the
      predecessor's generator when the seed is unchanged and reseeds otherwise; the
      distribution always follows the current `min` and `max`.
    - Tests: node_layout_storage.cpp checks `UniformNoise` and `UniformToPower` across
      argument changes.
  - *Not changed.* The `LearnedHrtfSource` and `BaselineFir256` nodes of the
    projects/simple_sine modules `q24_pan`, `q24_bspline_pan`, and
    `q24_icosphere_pan` read filter coefficients in `initialize()` from a fixed
    `static constexpr` `coefficient_path`, not from an argument. An implicit move keeps
    the coefficients the predecessor read; it differs from a fresh node only if that
    file changed on disk in between.
  - *Collaborator run (2026-11-08): 794 of 798 tests pass.* The three compensation
    reproduction tests fail as predicted, each with 7 mismatches in the first block
    after the cutover at 1024. `SeparateEventOutputAndInputStateNeedsNoTransitionRoot`
    expected its event consumer probe to start fresh; its `State` now moves
    implicitly, so the test now expects the successor's block to be the probe's
    second call. `FeedForwardEventFanInCompactCarryMigrates` builds its graph through
    `configure_package_definition_provider()`, so its nodes have no virtual node and
    still start fresh, as expected.

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
