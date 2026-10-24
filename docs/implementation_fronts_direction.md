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

**Can start independently:** yes.

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

The predecessor-input part cannot be implemented by copying
`ProjectGraph::current_generation_` into the ordinary compile request. That is the
latest logical graph, but `GraphExecutor` deliberately permits a pending generation to
be superseded before activation. The executor's active predecessor can therefore be an
older generation, and a transition program compiled against the logical predecessor
would be semantically wrong. The generations are already distinguishable inside
`GraphExecutor`: `realtime_active_` is the predecessor and `pending_generation_` is
only a replaceable candidate. The missing piece is a cold compiler-coordination path
which receives that active identity. Do not serialize graph edits merely to make these
identities coincide, and do not preallocate every possible transition suffix in the
steady compile.

**Coordination resolution (2026-10-18):** steady compilation remains
predecessor-independent. `GraphExecutor::prepare_stage()` now returns a cold staging
context containing the exact activated predecessor and intended successor generation.
The application-side staging coordinator will use that immutable context to request
transition specialization, then return the result with the same context to
`GraphExecutor::stage()`. Staging accepts replacement of any pending generation, but
returns `active_predecessor_changed` if a realtime activation changed the predecessor
in the meantime; the caller must then specialize again. This gives the compiler the
active identity without giving it mutable executor ownership or treating a pending
generation as semantic history.

The context currently tokens the active `CompiledGraph` identity. When the first
transition root lands, the same logical generation will be able to switch from its
transition realization to its steady realization. That realization phase is also part
of the semantic predecessor: `prepare_stage()` must snapshot it, and `stage()` must
reject a specialization if expiry changed it while compilation was in flight. Do not
infer the phase from project-generation equality.

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
pending):** GraphJit can now cold-specialize an already compiled steady successor from
the exact predecessor and realization phase captured by `GraphExecutorStageContext`.
For feed-forward project-rate `Sample` input history backed by a canonical persistent
ring, specialization appends private suffix rings, transfers inherited history at
cutover, mirrors newly produced samples into those rings while the transition root is
active, and emits transition and steady entrypoints in the same LLVM module. Both roots
share the steady layout prefix and authored node `State`; expiry changes only the
selected entrypoint. `CompiledGraph` retains the prepared semantic cutover and the
transition-phase realization inventory, while its weak predecessor token avoids pinning
obsolete generated code for the successor's lifetime.

`GraphExecutor` now tokens the active realization phase as well as the active compiled
graph. A specialization becomes stale if the predecessor expires from transition to
steady while compilation is in flight. Activation selects the transition root without
allocation, and callback completion switches the generation to its steady root once the
finite history horizon has elapsed. If expiry instead occurs after staging but before
activation, the executor discards that prepared generation at the quiescent boundary;
its suffix transfer can no longer be current and cold specialization must be repeated.
A newer revision can therefore specialize from the suffix inventory while the
transition remains active. The contiguous suffix allocation is still retained until the
generation retires, as already recorded above.

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

The revised execution-path order is:

1. Front 2 callback normalization and scalar/batch scheduling, including the deferred
   normalized block-replay consumer;
2. Front 3 node-facing `Region`/`Coverage` data representation over the already-landed
   persisted region/chunk substrate;
3. the remaining Front 5 generalized capture/publication work;
4. Front 8 application execution and system-audio integration;
5. the Front 16 measurement/baseline step required before structural optimization;
6. Front 12 scoped builder migration; and
7. the remaining Front 16 optimizations after the correctness architecture and
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

This front owns:

- construction and lifetime of the `GraphExecutor` app module plus its internal
  `RealtimeExecutor`, `BackgroundExecutor`, and `AsyncCapacityManager`;
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
- audio callback routing through `RealtimeExecutor`;
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

## Front 9: restricted configuration-expression language

**Primary ownership:** parse persisted/project-authored configuration source into owned
typed argument values without accepting arbitrary C++ execution.

The persisted syntax should remain C++-like because it needs natural typed literals,
aggregate construction, and nested initialization lists, but the accepted language is
intentionally much smaller than C++. This front owns the grammar, parser, type checking,
and evaluation model.

Required language capabilities include at least:

- scalar/string/enum literals as required by registered configuration signatures;
- nested braced initialization lists and aggregate/value construction;
- lists/arrays and other registered container/value structures as construction
  arguments;
- ordinary numerical arithmetic such as `+`, `-`, `*`, `/`, unary operations, and a
  small allow-listed set of pure mathematical functions;
- a small framework-provided reference/query vocabulary, potentially including forms
  such as `ref(user_authored_node_id)` and
  `select(virtual_node_name)[idx]...`;
- no arbitrary statements, mutation, loops, allocation APIs, filesystem/network
  access, arbitrary function calls, lambdas, templates, or general C++ execution;
- deterministic evaluation with no correctness dependence on how often an expression
  is evaluated or cached.

Exact grammar and spelling of the predefined functions are owned by this team. The
important contract is a deliberately restricted, auditable expression language rather
than handing arbitrary persisted source to Clang.

The evaluator must use the registered configuration signature/type information to
produce the same owned typed argument tuples already consumed by `NodeInstances`.
Provider-generated operations may still be used for clone/destroy/equality/hash and
for safe construction of provider-defined value types; that does not require compiling
arbitrary user-authored C++.

The expression/value representation should also support **argument-preserving node
replacement**. An `on_message` handler that replaces its own semantic node must be able
to inspect/reuse the predecessor's configured argument values and replace only selected
arguments rather than reconstructing the complete configuration from unrelated copied
captures. Lists and other structured argument values must remain ordinary first-class
values so module-node automation can evolve a dynamic set of construction arguments.

This front should expose a stable parsed/value representation that fronts 10 and 13 can
use for persistence and semantic replacement without duplicating parsers.

**Can start independently:** yes.

Detailed direction:
[Node Definitions And Instances Direction](./node_definitions_and_instances_direction.md),
[Project Graph Application Architecture](./project_graph_application_architecture.md),
and [Project File Serialization Direction](./project_file_serialization_direction.md).

## Front 10: generalized project persistence and graph mutation APIs

**Primary ownership:** the durable desired project graph and transport-neutral mutation
surface.

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

## Front 13: source introspection and semantic node interaction

**Primary ownership:** the typed semantic event/control layer inside the backend.

This front owns:

- making source introspection mandatory package infrastructure where required;
- validation and stable identity for declared `State`/`TockState` fields;
- member-pointer-based declaration validation;
- typed node-message handler discovery;
- generated event serializers/deserializers and frontend type metadata;
- stable `NodeAddress` and selector semantics;
- leaf and module-node handlers;
- ordered one-way node-to-node event dispatch;
- graph-revision-safe semantic addressing;
- node-local replace/reconfigure operations;
- exposing the predecessor node's current configuration arguments to a replacement
  handler and allowing selected arguments/structured subvalues to be replaced while
  preserving the rest;
- interaction journaling/commit integration without exposing realtime storage to the
  public handler context.

The actual configuration values and expression/value manipulation used by replacement
come from front 9. This front owns event semantics and replacement transactionality,
not the configuration grammar.

The live-observation/presentation telemetry API remains separate and may stay unresolved
until front 14 needs concrete capabilities.

## Front 14: presentation and persistent manual controls

**Primary ownership:** semantic presentation definitions and user-controlled dynamic
inputs.

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
- predictive JIT requests;
- constant/dynamic compatible code variants and stale-compile rejection;
- eventual live-observation capability/subscription APIs once concrete requirements are
  settled.

**Dependencies:** semantic identity/events from front 13; revision continuity from
front 6; later specialization benefits from front 12 and the mature executor.

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
- in-place event fan-in over a transient source whose last read is the merge;
- transient storage reuse/liveness improvements;
- immutable-value specialization;
- finer NodeInstances cache invalidation/reuse;
- measured builder/package reload optimization;
- runtime PGO experiments if profiling demonstrates a worthwhile target.
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
- Replacement at the graph-call boundary should reuse the existing
  successor-staging path (`GraphExecutorStageContext` and `GraphExecutor::stage()`) as
  far as possible.
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
