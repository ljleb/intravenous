# Automation And Background Planning Migration

_Implementation coordination, 2026-11-15. This is an implementation order, not
another normative contract. Current source progress is recorded below._

The governing decision is commit `d2e1d0ec`, especially
[Cyclic Automation, Event Projections, And Suspendable Coverage Propagation](automation_and_suspendable_coverage_direction.md).
The implementation baseline is `6826b784` (`automation clip v1`). The worktree
was clean when this comparison began. The earlier Stage 3 checkpoints describe
that baseline, not completion of this migration.

## 2026-11-16 superseding implementation destination

The November 16 removal of `TickReplayMode::input_union` was a documentation
and source regression, corrected by the
[2026-11-17 recovered contract](./tick_input_union_and_feedback_direction.md).
Restore acyclic input-union coverage and apply effective full live coverage only
to Tick members of feedback SCCs; exclude those SCCs from ordinary replay.
Earlier mistaken wording is retained
[historically](./historical/tick_coverage_before_intent_recovery_2026-11-17/automation_and_background_migration_plan.md).

Whole-Region `tock_coverage()` materialization and strict
`OutputRetention::persisted` retention are **superseded** by [Output Capabilities, Buffer Access, And Background Demand](./output_capabilities_and_background_demand.md). The
historical decisions and source checkpoints later in this file remain useful
as records of the staged implementation, not instructions to retain those
semantics. Former wording is preserved in the [historical predecessor](./historical/output_ports_before_computed_buffered_2026-11-16.md).

The complete follow-up migration must:

1. Rename authored Tick output shape to `SequentialOutputConfig`, expose
   Sequential outputs for Tick primitives and Region/Coverage outputs for Tock primitives,
   and replace `OutputRetention::{ephemeral,persisted}` with
   `OutputRealization::{computed,buffered}` across static traits, reflection,
   configured graphs, serialization, GraphJit plans and public tests.
2. Preserve `TickReplayMode::{disabled,input_union,full}` and distinguish
   computed values from logical coverage. Acyclic input-union nodes keep sparse
   coverage. Tick members of feedback SCCs have effective full live coverage
   and are explicitly ineligible for ordinary contextual replay. Remove only
   the recursive coverage proof, not input-union semantics.
3. Add windowed versus complete-buffer Region/Coverage input capabilities.
   Reject Coverage-to-Region and computed-to-complete-buffer connections;
   make explicit materialization nodes author a bounded buffered result.
   Initially require complete-buffer Region/Coverage inputs on Tick primitives,
   including outputless sinks; prepare their complete selected Regions before
   execution. Tock consumers may declare windowed access. Do not add a
   playback-window callback as part of this initial implementation.
4. Replace whole-selected-Region allocation with demand-scoped windows,
   preserving Region identity, overlap and output `IndexCoverage`. Extend
   background storage/binding frames and reverse dependency requests to
   separately track requested, materialized and published-valid positions.
5. Permit partially materialized buffered versions, require coherence and
   per-window completion, and move cache/eviction choices to the executor.
   Retain authoritative recording information separately from disposable
   prepared caches. Share a single background-managed page/window storage
   realization between Tick/Sequential/buffered recordings and Tock/buffered
   outputs rather than duplicating their backing and publication machinery.
   Make sparse virtual mappings an optional backend with configurable limits
   on one mapping's byte span and aggregate reserved virtual address space;
   separately budget process-wide VMAs/physical-run fragmentation, physical
   backing and realtime-resident pages. Preserve run-based mapping assembly,
   implement pin-safe reclamation/reuse of obsolete `memfd` backing after old
   snapshots retire, and select initial limits from measured fragmented
   overwrite and sparse Tock workloads rather than fixed port semantics.
   Fall back to bounded/segmented storage only where input access permits.
6. Stage future Sequential consumer windows with a bounded rolling buffer;
   prepare every realtime Random Access window in advance and reject
   unbounded/dynamic realtime reads that cannot be prepared.
7. Migrate the automation interpreter to Tock/computed Region/Coverage
   generation with exact empty/hold/bezier coverage and windowed computation.
   Update tests after the final API and scheduler behavior are in place.

This document is implementation coordination, not an alternate normative
port configuration reference.

## What changes

- A point describes an outgoing `empty`, `hold`, or automatic `bezier` segment.
  Zero points produce empty coverage. `empty` means no coverage, not zero-valued
  samples. The last point connects to the first in circular index order.
- `CurvePointEvent` retains value, outgoing policy, and tension. `SampleEvent`
  does not contain enough information. Point IDs remain editing identities.
- Exact output coverage can depend on input contents. Forward and reverse
  callbacks can therefore suspend while the runtime materializes explicitly
  requested discovery inputs. They retain their different semantic roles.
- Successful reads record invalidation dependencies. Explicit unresolved waits
  name scheduling prerequisites. These are different records and lifetimes.
- Tock sees only its finalized requested logical input Regions. Discovery reads
  and wider cached storage must not leak into value-production bindings.
- Region/Coverage inputs preserve absence and never substitute manual or
  neutral values. Sequential inputs choose their declared neutral/manual
  fallback only on known absence. Pending discovery is not an uncovered result.

The useful parts of the existing work remain: typed messages, durable replacement,
gesture history, presentation-owned editing, immutable point arrays, sparse event
storage, modular neighbor lookup, and requested-window Tick replay. A finite
point-storage Region may remain. It does not define active curve intervals.

## 1. Complete requested-only Tock bindings

This is the first independent correctness feature. Audit and finish the path
from reverse input requirements to normalized logical Region selections and
the node-facing binding. Preserve full selected Regions, overlap, duplicate
ranges, and valid zero-event Regions. Derive node-visible positional coverage
from the explicitly selected Regions; do not expose additional stored Regions.
An empty resolved selection must remain empty.

Current implementation points:

- `BackgroundEvaluationTransaction::Impl::binding_selection()` fills
  `BackgroundEvaluationCallFrameSelection::binding_coverage` from required
  positions, while `binding_regions` selects intersecting whole input Regions.
  The two records need one consistent, explicitly resolved request.
- `BackgroundEvaluationCallFrame::seal()` assembles callback views from storage
  and checks propagated Region lists. This already implements part of the
  boundary; it is not a reason to assume every path meets the new invariant.

Verify equivalent results with cold storage and with unrelated extra Regions
already materialized. Cover continuous values, events, overlapping/duplicate
Regions, empty selections, and different requests in separate batch lanes.
Do not obtain requested-only views by clipping selected source Regions.

## 2. Implement suspendable propagation and its scheduler together

This is the largest prerequisite and one coherent runtime feature. Do not land
a coroutine return type that the existing fixed-phase scheduler cannot execute.

The current `BackgroundPropagationWorkspace::prepare()` calls forward roots and
then reverse roots. `BackgroundEvaluationTransaction::Impl` obtains that prepared
result before creating/sealing evaluation storage and calling the generated
evaluation root. Propagation contexts currently expose coverage/requirements,
and scalar propagation traits require `void`. Adding a read method alone cannot
materialize discovery data under this ordering.

Internal implementation order:

1. Define explicit information requests and results: output port, selected
   semantic/data version, coverage versus contents, and needed positions/logical
   Regions. Distinguish unresolved coverage from successfully known-empty coverage.
2. Give the private candidate ownership of request readiness, successful-read
   invalidation dependencies, and explicitly awaited unresolved prerequisites.
   Retain its selected immutable input environment and producer-queue prefixes.
3. Add scalar propagation task normalization through authored contexts, traits,
   reflected package callbacks, and GraphJit bindings. Normalize synchronous
   scalar and existing batch callbacks without requiring coroutine frames.
   Define start/resume/completion/error/cancellation and ownership explicitly.
4. Let the transaction interleave ready forward/reverse operations with the
   existing ordinary Tock/replay materialization operations. Track readiness per
   output port, information kind, and required region. Resume only when named
   prerequisites progress. Batch compatible ready synchronous work.
5. Replace scheduling assumptions that require a single propagation visit or
   whole-node readiness. Detect cycles among actual unresolved requirements,
   including coverage/content dependencies. Do not reject useful independent
   outputs because another output of the same node is pending.
6. Commit only after all candidate obligations are complete. Failure,
   supersession, and cancellation destroy suspended tasks while their contexts,
   selected views, configuration, and JIT code are still alive. No discovery
   result becomes a partial playback publication.

Use small authored test nodes to verify content-dependent forward coverage,
reverse discovery, grouped independent waits, per-output progress, true cycles,
false whole-node cycles, invalidation after successful reads, cancellation, and
unchanged synchronous/batch behavior. Ordinary `tock_coverage()` remains ordinary
value production; the audio thread never runs these planning tasks.

## 3. Complete the new automation behavior end to end

These are dependent parts of one automation feature, not separate claims that
the user-facing change is complete. The dependency order within it is:

1. Settle duplicate-position semantics and the concrete interpolation/tension
   convention before freezing payload serialization. The design explicitly
   leaves the tangent estimator, tension mapping, and behavior beside gaps or
   holds open. Propose a monotone/clamped automatic cubic with an exact-linear
   setting; confirm any choice that has no clear unique best solution.
2. Add `CurvePointEvent` through event declaration, validation, persistence,
   package/JIT transport, and single-type point-source/interpolator ports. Keep
   editing IDs separate. Do not fabricate conversions to unrelated event types.
3. Make forward propagation request and inspect the actual point input, then
   publish the union of nonempty outgoing segments. Use the same segment
   interpretation for value production. Retain whole finite point Regions as
   explicit reverse requirements when needed for neighbors and tangents.
   Transformed upstream events must affect both values and coverage; do not read
   `PointSource` construction data as a shortcut around input discovery.
4. Implement the destination manual-value selection policy in connection/
   storage planning, materialization, and Sequential lowering. Only Sequential
   inputs have a declared neutral/manual absence policy; Region/Coverage inputs
   retain sparse data and have no fallback accessors. Keep this distinct from
   additive fan-in and temporary manual override. Do not recognize builtin node
   IDs. Preserve existing dynamic-control, history, conversion and transition
   paths, and distinguish known absence from missing covered materialization.
5. Update point construction, semantic messages, generated bindings, and the Vue
   presentation together. Expose segment policy and tension, display gaps as
   gaps, and keep creation defaults presentation-owned. Retain reliable accepted
   reads and one undo operation per gesture. The visual curve and backend must
   use the same interpolation convention.

Tests must include zero/one/multiple points, all segment kinds, endpoint and
cyclic-seam membership, equal-position policy, irregular spacing, clamped cubic
values, and exact linear tension. Exercise a point source through an event
transformer before the interpreter. Verify a connected destination uses its
independently editable manual value only on known-uncovered positions, including
graph revisions and pending discovery. Re-run the existing control and transition
regressions; settled/live controls must remain usable.

## 4. Extend event ports and projections after the automation path works

Multi-type event ports are an accepted destination but are not a dependency of
the one-type `CurvePointEvent` path. Implement set-subset direct compatibility
and variant-style consumption as a separate complete port feature. Keep
implicit conversions disabled for those ports until their general rule is
settled.

`EventConversionRegistry` is already a graph of explicit directed edges. Extend
that mechanism when real consumers justify projections rather than replacing it
with a hierarchy or requiring every new type to connect to every other type.
The proposed Sample/Trigger/Boundary projections are candidates, not mandatory
edges. A Boundary projection that emits repeated states does not claim to
produce a minimal boundary sequence.

## Position in the Vue stages

This migration is the next dependency of the revised Stage 3 automation feature.
Do not add more controls to the old full-circle linear behavior and call that
completion. Stages 4–5 observation still follow a working Stage 3. The previously
agreed `Node::State` lifecycle cleanup remains before observation; it is not a
new prerequisite for the requested-only binding fix.

No code, build, or test was changed or run for this planning pass. Source fallback
was used because the available knowledge-graph generation predates these commits.


## Source checkpoint: requested-only bindings, 2026-11-15

Implemented in source; builds and tests remain owner-run. The original planning
checkpoint above changed no implementation and ran no tests.

- `BackgroundPropagationWorkspace` retains original input-selection requests
  separately from normalized whole-Region input requirements. Prepared results
  retain both intrinsic available input Regions and explicitly requested Regions.
- `BackgroundEvaluationTransaction` binds the requested Region list directly.
  Expanded producer completion can materialize overlapping Regions without adding
  them to the consumer's selected list.
- `BackgroundEvaluationCallFrame` selects only those Region records, checks that
  visible coverage equals their positional union, and retains duplicate logical
  continuous Regions even if they share immutable bytes. Extra cached views are
  omitted. Empty input selections remain empty.
- New light regressions cover sample/event reverse normalization, wider upstream
  materialization, retained coverage, empty selections, aliased duplicate sample
  Regions, zero-event Regions, and independent event selections for two nodes
  sharing storage. No build or test has been run by the agent.

At this checkpoint, suspendable propagation and the revised automation implementation
had not started. The tension default was still open; the decision below supersedes
that open question.


### Tension convention accepted, 2026-11-15

The owner selected tension in [0,1], blending exact linear interpolation at 0
with the clamped automatic curve at 1. Newly added Bézier points default to
1. This settles the audible default and numeric mapping; it does not claim
that the new event payload, segment evaluator, or UI has been implemented.
Scheduler work resumes from the requested-only binding source checkpoint.

### Suspendable propagation implementation in progress

The initial worktree checkpoint contained `CoverageTask`, erased coroutine
ownership, typed request handles, grouped waits, scalar callback traits, and
reflected planning-context transport. Suspendable adapters retain an owned
callback context; synchronous callbacks keep their stack context. At that
checkpoint these were unfinished parts of step 2. Host scheduling, discovery
materialization, dependency tracking, cancellation, and tests were still missing.

Discovery must retain its own explicit input selections. Exploratory reads cannot
be added to the final Tock reverse requirements. Both use the selected transaction
snapshot and fixed producer prefixes; neither may publish partial data.

### Source checkpoint: scheduler and discovery integration

Implemented in source, with verification still pending the owner's build/test run:

- `BackgroundPlanningScheduler` distinguishes created requests, successful reads,
  and explicitly awaited unresolved prerequisites. Grouped waits do not wait on
  unrelated requests. It rejects unnamed suspension and reports unresolved
  dependencies with node, input port, information kind, and requested ranges.
- `BackgroundPropagationWorkspace` uses the scheduler for forward/reverse work.
  An input's unknown metadata cannot be read as an empty result. Synchronous
  callbacks retain their generated batch roots; suspendable scalar callbacks
  retain an owned context. Named `request_input_coverage<"name">()` and
  `request_input<"name">(positions)` return awaitable results.
- Coverage becomes available per published output port, independently of another
  pending output of that callback. A published output is final across a
  suspension: changing its coverage, Region list, or invalidation afterward is
  rejected. A synchronous getter encountering unknown metadata defers that
  operation; a coroutine must explicitly await unresolved information.
- Discovery reverse scopes have independent requirement accumulators and share
  the selected candidate's scheduler. `BackgroundEvaluationTransaction` reuses
  ordinary sealed Tock/replay evaluation to produce their read-only input views.
  Their storage remains alive through suspended consumers. Discovery does not
  publish to playback or widen the final Tock selection.
- Successful metadata/content reads are retained in semantic coverage state and
  remapped by stable input identity during migration. Existing graph routing
  remains conservative for invalidation. Task frames are destroyed before their
  contexts and retained discovery views on failure/cancellation.
- Added light scheduler and propagation tests for grouped waits, independent
  information progress, true cycles, invalid suspension, cancellation order,
  synchronous batching, content-driven coverage, and final input selection.
  Added a native transaction regression using ordinary Tock discovery and one
  shared-package GraphJit regression exercising authored forward and reverse
  coroutines with named typed requests. These tests have not been run by the
  agent.

The new `CurvePointEvent`, interpolation, destination manual-fallback policy,
messages, and presentation remain step 3 work. The existing automation node still
has its previous all-circle linear behavior; it does not yet satisfy the new
normative contract. Event type sets and optional projections remain later work.

### Step 3 decisions accepted, 2026-11-15

- Preserve coincident-point jumps in event order: the first value ends the
  incoming segment and the last value starts the outgoing segment. Intermediate
  zero-length segments contain no samples.
- The access-mode fallback decision recorded here was subsequently replaced
  by the Sequential-only resolution contract below. The superseded wording is
  retained in the historical record linked there.

The previous open questions and unaccepted recommendations are preserved in
[historical documentation](historical/automation_manual_fallback_before_random_access_policy_2026-11-15.md).

### Final absence policy accepted, 2026-11-15

Region/Coverage inputs retain sparse, separately identifiable supplied Regions;
out-of-Region reads are invalid. They have no manual knob, fallback configuration,
`uncovered_value()` or fallback `read()` interface. Sequential inputs alone
resolve absent data through their declared neutral/manual policy. The default
remains neutral; manual fallback is an opt-in for Sequential Sample inputs.
Fallback is not fan-in and does not change upstream coverage. Known absence
must remain distinct from covered data that has not been materialized.

The point-group, tension, and stable ordering decisions remain in force.
Source cleanup removes the unfinished random-access resolved-read additions,
while preserving requested-only binding and suspendable discovery work. The
next implementation step is Sequential fallback followed by CurvePointEvent,
exact segment coverage/evaluation, and the messages/presentation migration.
Earlier proposals and the incomplete source checkpoint are retained
[historically](historical/automation_random_access_fallback_before_sequential_resolution_2026-11-15.md).
No new build or tests have been run by the agent.

### Friction observed: Sequential fallback at the Sequential execution boundary, 2026-11-15

_Observation, not a requirement. Revised the same day after owner review; the
representation remains open._ The agent has not changed source for this step.
Codex's last source change adds `SequentialFallback {neutral, manual_control}` to
`SampleContinuousValueExtension::BoundaryInputOptions`, `SequentialInputOptions`,
and `SequentialInputMetadata` (including its binary form). No planning, lowering,
or runtime code reads it yet.

Owner framing: this is an unimplemented part of the existing coverage semantics
at the Sequential execution boundary, not a missing product decision. Three
states stay distinct: known uncovered (apply the declared fallback), covered and
materialized (use the upstream value), and covered but not materialized (missing
data; a separate playback-underrun concern that never activates manual
fallback). Warm and cold caches must not change whether a position is covered.
Fallback applies only where the **aggregate** of all contributions reaching
the destination is uncovered, after their latency, detach, pace, and channel
routing. Coverage transitions may occur inside a block.

Traced source facts:

- `SampleConnectionPlan::source_channel_timings` gives each source channel a
  `PlannedDeliveryMechanism`. Sequential destinations receive
  `tick_to_sequential` (same-slice realtime) and `tock_to_sequential`
  (background data through `TickInvocationCall` slots) contributions. No plan
  record carries source coverage.
- Contributions combine in three places: linear `ChannelMix` terms in
  `compose_samples()`; slot values merged with the realtime operand by the
  node's `combine_sequential_input_values` in `stage_sample_timelines()`; and
  participating manual controls in `stage_manual_control_sample_timelines()`.
- `random_access_port_data_direction.md` makes `neutral_value` the identity of
  an associative, commutative `combine_values` and reduces fan-in from neutral.
  An uncovered contribution is therefore equivalent to a neutral contribution.
  Its Coverage-to-Sequential rendering leaves positions with no contribution at
  neutral; the new policy changes only that last step.
- `TickInvocationFrame::SequentialSampleSlot::populate()` clears to neutral and
  copies published and materialized Regions. `TickMaterializedSampleInput::coverage`
  is the union of materialized Regions, not upstream semantic coverage, so the
  slot cannot tell absence from unmaterialized covered data.
- Manual values come from `ManualControlPlan` and the NodeStorage
  `ManualControlRuntimeState`: settled constants, or dynamic interpolation from
  `effective_bits` to the latched target over a callback. A module public
  input's control (`boundary_source`) carries channel coefficients and a delay
  ring from `ManualSampleRoutePlan`. Non-participating controls are not staged.
- `realtime_root.cpp`, `SampleStoragePlan`, and `TickMaterializationSnapshot`
  carry no producer coverage. `LinearCurve::propagate_forward_coverage()`
  publishes only to background planning.

Rejected as worded: masking each destination from `TickMaterializationSnapshot`
with one lookup per block. Masking a single connection ignores the other
contributions, and one lookup per block ignores routing and in-block
transitions. A per-sample validity sideband on every Tick output, or feeding
live consumers from background replay, remain worse alternatives.

Direction under investigation: coverage becomes part of a Sequential
contribution's execution semantics through connection planning, storage
planning, and composition. A destination resolves its fallback where the
aggregate contribution coverage is empty, using version-consistent coverage
pinned with the data it describes. Manual fallback reuses the existing control
state; it is neither a fixed copied value nor a fan-in contribution.

Second owner review, same day. Rejected: an agent proposal to classify
contributions as dense or sparse, and to make replayability depend on
`SequentialFallback`. Direction:

- add no source classification. Exact output `IndexCoverage`, `InputAccess`, and
  `OutputProduction` already carry the facts. Plan records such as
  `SampleCompositionInputPlan` identify which output's coverage applies; the
  runtime supplies that coverage version-consistently, outside compiled plans
  and separate from sample storage and materialization availability;
- route coverage with the rules of `derive_input_coverage()` (intersection
  within one projection's source channels, union across projections and
  connections, read-latency shifts), shared with realtime resolution rather
  than reinterpreted. Background and realtime must agree whether a
  contribution exists;
- resolve covered contributions with `combine_values`, then fill only empty
  aggregate positions. Fallback is never staged as a contribution. Separate
  obtaining the manual value (authoritative `ManualControlRuntimeState`,
  shared interpolation) from deciding how it participates;
- replayability follows the manual-control state actually used, not the enum;
  replay must name the semantic version and control state it evaluates;
- before `CurvePointEvent`, test with a producer publishing disconnected
  coverage, a module-boundary manual control, overlapping contributions, and
  coverage that changes between published versions.

Open question: can every contribution's coverage come from the same committed
version as its sample values? Trace so far:

- Tock outputs: final commit promotes prepared coverage together with the data
  snapshot, so the mailbox pair can carry both. Today
  `SequentialSampleSlot::populate()` receives only materialized and stored
  Regions.
- Tick outputs with authored forward coverage (for example `LinearCurve`):
  the transaction computes coverage from the same Random Access views that the
  live `tick_block()` reads from that pair. This is consistent if both come from
  one pair and authored coverage depends only on semantic state and inputs.
- Tick outputs with compiler-generated coverage: `replay_forward_coverage()`
  published the union of input coverages, Sequential inputs included. This
  conflicted with existing intent rather than raising a new question: §4 says
  Sequential inputs provide a value at every position, so absence stops at a
  Sequential input (owner clarification, now explicit in §4). The union also
  gave a replayed node without inputs empty coverage. Resolved below.
- Ordinary live Tick outputs outside `BackgroundEvaluationPlan` have no coverage
  record. Activation of a generation before its first committed transaction
  has not been traced.

### TickReplayMode accepted; source checkpoint, 2026-11-15

Owner decision, recorded normatively in
[automation direction, section 4](automation_and_suspendable_coverage_direction.md#4-absence-and-sequential-fallback):
one node-level `TickReplayMode {disabled, input_union, full}` replaces the
boolean `intrinsically_replayable`. It sets eligibility and selects generated
output coverage (union of input coverages, or the whole circle). Authored
forward coverage takes precedence; there is no per-output field. An earlier
same-day agent change that made generated coverage always full, and the pending
per-output proposal, are preserved
[historically](historical/replay_coverage_before_tick_replay_mode_2026-11-15.md).

Implemented in source; build and tests remain owner-run:

- `TickReplayMode` in `node/tick_replay_mode.h`. `tick_replay_mode_v<Node>` reads
  `Node::replay_mode`; a node still declaring `intrinsically_replayable` fails
  to compile. `intrinsically_replayable_v` derives from the mode.
- The mode replaces the boolean in `NodeCompilerRecord`, the reflected node
  description, the builder's bundle records, the `ConfiguredGraph` binary
  archive (format version unchanged), `PlannedGraphNode`, the lowering
  implementation record, and `BackgroundNodePlan`.
- `replay_forward_coverage()` again publishes the union of input coverages for
  `input_union`; `replay_full_forward_coverage()` publishes the whole circle for
  `full`. Both report input changes, or their whole coverage after a local
  change.
- `LinearCurve` declares `input_union`; its authored callbacks decide coverage.
  Test nodes without inputs declare `full`; the others `input_union`.
- `GeneratedReplayCoverageFollowsTheDeclaredTickReplayMode` checks an
  `input_union` node covering only its source's [20, 24), a `full` node and a
  `full` generator covering the circle, a cold-source requirement of exactly
  [20, 24), and computed values where the `full` node's input is uncovered. A
  static-validation case rejects the removed boolean.

- Background replay resolves its Sequential inputs over the scheduled
  positions. `PreparedCoveragePropagation::input_coverage()` exposes each
  input's upstream coverage (`derive_input_coverage()`), and the transaction
  passes it per binding slot as
  `BackgroundEvaluationCallFrameSelection::binding_upstream_coverage`.
  `prepare_replay()` reads selected data, writes the input's neutral value on
  known-absent positions (no events for event inputs), and still fails on
  covered positions outside the selection. Before this, a `full` node replaying
  over uncovered upstream failed with "background replay sample input is not
  fully selected". Manual-control fallback will resolve at the same place.

The live path does not yet honor `input_union` coverage: realtime Sequential
consumers still receive every computed sample. That is part of the pending
coverage-aware Sequential resolution.

This resolves the earlier step-3 observation, now preserved historically: a
pointwise Tick event transformer between the point source and the interpreter
declares `input_union`, so its output keeps the finite point Region and the
interpreter's whole-coverage reverse requirement stays finite.

### Staged-tree test fixes, 2026-11-15

From the owner's run of the staged tree (185 failures: 177 not run behind
`GraphJitSharedRuntimeFixture.BuildPackage`, 8 failed):

- `BuildPackage` aborted with `LLVM ERROR: Do not know how to promote this
  operator!` in the test process. The finalized package LLVM from
  `buildThinLTOPreLinkDefaultPipeline(O2)` still held the unsplit coroutines of
  `SuspendableCoverageProbe`'s propagation callbacks (`presplitcoroutine`,
  `llvm.coro.suspend`), and the package's configuration ORC JIT generates code
  without IR passes. `prelink_optimize_package()` now runs CoroEarly, CoroSplit
  and CoroCleanup in a `CoroConditionalWrapper` after the pre-link pipeline and
  rejects any remaining pre-split coroutine. This is the first package with
  coroutine callbacks; the builtin package was unaffected.
- `BackgroundEvaluationCallFrame.OwnsOperationFramesAndTockBindings` predates
  the requested-only binding: its input binding coverage was narrower than its
  whole selected Region. The test now binds that Region's union.
- `GeneratedReplayCoverageFollowsTheDeclaredTickReplayMode` exposed an older
  replay addressing defect, found under gdb by dumping the replay storage at
  `finalize_replay()`. A Tick wrapper addresses each continuous binding at
  `sequential_begin_index` plus the frame offset, while `prepare_replay()` and
  `finalize_replay()` address replay storage at `index & (capacity - 1)`. The
  generated replay loop never set `sequential_begin_index`, so every region's
  invocation used slot 0. Earlier replays passed only because each had one
  region beginning at a multiple of its capacity. The loop in `lowering.cpp`
  now stores each region's begin into every continuous input and output binding
  of the replay context before invoking the Tick wrapper.
- Six persisted `GraphExecutorFixture` tests committed without evaluation. A
  persisted demand records its requirement without activating the producer;
  demand expansion then activated it for the same range, but the staged
  `require_output()` activated only when the requirement grew. It now also
  activates a producer whose reverse operation is not yet scheduled.

### Review after `da2ceb5e`, 2026-11-15

The owner's current `build-release/Testing/Temporary/LastTest-verbatim.log`
records 1006 passing tests, with no failures, in 53.31 seconds. This result
supersedes the failed-run checkpoint above. The reviewing agent did not run
another build or test suite.

A focused source review covered the replay-mode transport, coroutine lowering,
replay addressing, and producer activation changes in `1961d3ab` and
`da2ceb5e`. It found no concrete defect in those changes. This is evidence for
continuing from them, not a claim that the automation migration is complete.

The next coherent feature is coverage-aware Sequential resolution, across live
Tick execution and background replay. Its scope includes:

- Exact output coverage selected with the sample data's generation and version.
  `TickMaterializedSampleInput::coverage` continues to describe materialized
  Regions. It cannot establish whether an upstream value exists.
- Connection routing and timing shared with `derive_input_coverage()`, including
  the intersection of sources within a projection and the union of contributions.
- Neutral or manual fallback only where the combined contributions are known
  absent. Manual values use the existing control state and interpolation.
- Module public inputs, live overrides, graph activation, and code replacement.
  Covered data that are unavailable remain distinct from known absence.
- Regressions for disjoint coverage, overlapping contributions, module routing,
  changes between published versions, and agreement between live and replayed
  values.

`SequentialFallback` remains a declaration without execution support.
`LinearCurve` still implements the previous cyclic linear behavior. The new
`CurvePointEvent`, outgoing segment policies, interpolation, messages, and
presentation follow Sequential resolution. Variant event ports remain a later
feature.

### Sequential resolution implementation in progress, 2026-11-15

The active feature is coverage-aware Sequential resolution in live Tick and
background replay. The normative contract remains section 4 of
`automation_and_suspendable_coverage_direction.md`.

The first implementation change carries the prepared transaction's exact output
coverage in `TickMaterializationSnapshot`, indexed by output-change accumulator.
`TickInvocationWorkspace` exposes it through `TickInvocationCall` only when the
snapshot matches the generation and pinned data version. An initial snapshot
without these entries means unknown coverage, not known absence. This transport
is incomplete until connection resolution uses it.

Source tracing found two prerequisites within the same feature:

- `populate_background_topology()` currently enables Tick forward coverage only
  for contextually replayable nodes. Live Sequential connections also need the
  declared coverage, independently of background replay eligibility.
- Code-only replacements retain the structural generation's background plan
  and `TickInvocationWorkspace`. Coverage references and manual fallback state
  must therefore remain valid across replacement, or the replacement must use
  the existing structural transition path. Matching byte sizes alone cannot
  establish that compatibility.

Still pending: coverage planning for live connections, contribution masking,
neutral/manual selection, module-boundary resolution, replay control-state
selection, compatibility checks, and feature tests. No build or test was run.

#### Replay declarations and persisted outputs

The owner clarified on 2026-11-15 that replayable Tick nodes cannot declare
persisted outputs. `replay_ports_are_valid()` now rejects either a continuous
or event output whose retention is persisted. Both `IV_NODE` and internal
node construction report the ephemeral-output requirement. A new static
validation fixture covers `input_union` Sample and `full` event declarations.
This does not claim completion of Sequential resolution. Builds and tests
remain owner-run.

The owner further confirmed that this is an output-declaration rule only.
Incoming connections from another node's persisted output remain supported.
They are recorded dependencies for replay, not a reason to reject the
replayable consumer.

#### Module-input resolution: representation friction

Implementation observation, 2026-11-15. The current feature assumed that exact
coverage could accompany the existing flattened channel routes. That is enough
for a source projection, but not for resolving a public module input before its
internal routing.

`build_structural_sample_routes()` removes the module boundary while producing
`ResolvedSampleChannel::contributions`. `ManualSampleRoutePlan` retains a
control index, target, coefficient and delay. It does not retain the public
input's combined signal or a stage that resolves its known gaps. Several public
inputs and ordinary sources can subsequently merge at one concrete input.

For a sparse source connected to a public Sequential module input, fallback
belongs at that public input, before its internal conversion and fan-out. A
fallback test on the final leaf input cannot reconstruct that distinction from
the combined samples. Adding the manual value as another contribution also
violates the accepted contract.

Two implementation choices are under owner review:

1. Retain Sequential module-input resolution as an internal planning stage.
   Storage planning can share or inline its result and use existing timeline
   migration. This requires changes to structural routing, execution planning,
   and their storage/lowering interfaces.
2. Preserve flattening and carry each boundary's coverage and fallback
   expressions through every derived route. This avoids a separate stage but
   makes conversion, fan-out and temporal routing responsible for evaluating
   those expressions without losing their grouping.

The recommendation is the first choice. Retaining the semantic stage does not
require a visible project node or an unconditional extra buffer. Its physical
representation remains a storage/code-generation decision. No such stage has
yet been implemented, and the choice is pending.

The current source is an unfinished feature, not a tested checkpoint. Besides
the earlier transport changes, it now contains normalized coverage predicates,
coverage references retained through sample composition/detach planning, and
initial composition masking. Direct input resolution, manual fallback, module
resolution, event filtering, coverage selection for live versus recorded input
views, and code-replacement compatibility still need completion. The static
persisted-output declaration check is separate and complete in source. The
added tests cover that check and snapshot transport only. No build or test was
run; `git diff --check` passes.

#### Internal module-input resolution stage selected

Owner decision, 2026-11-15: retain Sequential module-input resolution as an
explicit internal planning stage. The alternative of carrying fallback
expressions through flattened routes is no longer the selected direction.
The friction report above remains a record of why the representation changes.

The stage resolves the module's combined public input before internal channel
conversion, fan-out and temporal routing. It is not a visible project node or
a new authored leaf-node API. GraphJit can share or inline its work and choose
its storage through the existing planning machinery. Implement this stage
before finishing leaf fallback, so module controls do not become a separate
fallback mechanism.

#### Public module-input access declarations selected

Owner decision, 2026-11-15: public module inputs explicitly declare Sequential,
Region or Coverage access. Sequential remains the default, consistent with
leaf input authoring. Region/Coverage pass-through definitions must declare
their access rather than relying on flattening to discard an incidental
Sequential label.

This resolves a further source mismatch: `make_boundary_input_config()` always
constructs a Sequential input, while `multi_region_boundary_child_module` uses
that input to forward Regions to a Coverage consumer. Adding an unconditional
Sequential resolution stage under the old API would destroy that sparse view.

Add the public access declaration API and preserve it through configuration,
then retain resolution stages only for declared Sequential module inputs.
Region/Coverage module inputs preserve sparse Region semantics and expose no
manual controls. This authoring/API prerequisite now precedes the internal
resolution-stage lowering. No access declaration changes have been tested yet.

#### Public input declaration API source checkpoint

`GraphBuilder::input()` and `SubgraphBuilder::input()` now accept explicit
access tokens for named, unnamed, typed continuous and channel-typed inputs.
The options type follows the declared access. Existing concise declarations
remain Sequential. Event declarations also accept access tokens, and the
session transports the complete `EventInputConfig` instead of recreating a
Sequential event input from its name and type.

The `multi_region_boundary_child_module` fixture now declares Coverage access
for its public sample and event inputs. A new light `GraphModules` regression
checks public access, Sequential fallback metadata, nested subgraphs, channel
layout, event access, and configured-graph archive round trips. The shared JIT
fixture's five source fragments remain below their 60 KiB limits. No build or
test was run.

One semantic question is pending before stage lowering: when a Sequential
module input supplies fallback, does its internal stage present a covered
value to an `input_union` child, or preserve the original source absence while
passing the resolved value? The first makes the module input a dense internal
signal. The second preserves the child coverage of the equivalent leaf case
but requires retaining resolved values separately from source-coverage
provenance through internal routing. Neither outcome is yet implemented.

#### Coverage after Sequential module-input resolution selected

Owner decision, 2026-11-15: a Sequential module input's resolved signal is
covered at fallback positions. An internal `input_union` consumer sees those
positions as covered and can produce output there. The alternative of carrying
original absence through the stage is not selected.

The stage therefore produces a dense internal Tick signal. This does not
change the original upstream output's coverage or synthesize Regions on that
output. Region/Coverage public inputs still preserve sparse views. The
compiler can lower a Sequential input stage as an internal producer with one
input and one output, retain its public-input identity for controls and
migration, and share or inline the stage's work.

#### Internal resolution-stage implementation checkpoint (unfinished)

Source work, 2026-11-15, following the covered-after-resolution decision:

- `graph_jit/module_input_resolution.h::resolve_module_inputs()` now builds a
  private compiler copy of the configured graph. It appends one concrete
  execution stage per public Sequential input and redirects incoming routes,
  internal boundary sources, and `ConfiguredManualControl::input` to that
  stage. Authored handles and the project graph remain intact. Public Region
  and Coverage inputs remain structural routes.
- `NodeBundle::resolved_module_input()` identifies the public owner of a
  compiler stage. `NodeImplementation` carries that origin explicitly. These
  stages have actual compiler-owned native kernels and imports, no fabricated
  package revision, registered definition, or compiler record.
- Continuous stages copy resolved values at their solved pace; event stages
  forward their resolved event windows. They declare `TickReplayMode::full`,
  use ordinary storage and callback planning, and can enter the existing
  generated replay path. Neutral initialization and combination use the
  registered continuous value extensions.
- `PlannedGraphNode::resolved_module_input` and
  `RealtimePortStateRequirement::resolved_module_input` retain public-input
  provenance for migration. The execution input is port 0; its public owner
  can have a different semantic input index. Internal stages do not acquire
  invented authored concrete-member indices.
- Direct Sequential Sample planning now requests a private input timeline
  where producer coverage can be absent. `stage_sequential_sample_resolution()`
  copies ordinary values or supplies fallback, before applying live manual
  interpolation. Connected inputs with provably full sources do not acquire
  a fallback-only manual timeline. Ordinary dense DSP specialization remains
  available; sparse resolution uses the existing specialization staging call.
- Background replay now retains a selected Sample fallback value separately
  from its neutral value. The selected fallback can be a configured manual
  value; live interpolation still reads `ManualControlRuntimeState`.

This is an incomplete source milestone, not a delivered feature. No build,
tests, npm checks, or typecheck were run. `git diff --check` passes. Compilation
has not been established, and this working tree is not ready for a suite run.

Known remaining connections inside this same feature:

1. Finish the coverage-cycle decision below before completing live forward
   coverage scheduling. Keep Tick production and Coverage access terminology
   distinct.
2. Publish a coherent initial live coverage selection before a root that needs
   it can execute. `RealtimePersistedState::capture_initial()` currently pins
   an empty materialization, while the new root requires known coverage.
   Unknown entries must not become fallback merely because background
   preparation has not finished.
3. Distinguish current Tick coverage from selected recorded coverage when a
   replayable processor consumes a persisted producer. A live Sequential edge
   reads freshly computed values; replay reads the selected recording. The
   current shared output-coverage span does not yet settle the transitive case.
4. Complete projection-level masking for mixed realtime/background inputs,
   event contribution masking, typed fallback, and replay of participating
   manual contributions. The new target resolution pass alone is insufficient
   for a mixed input with a nonzero neutral value.
5. Complete tiled module event-target expansion, audit all explicit sparse
   module-input declarations, and verify compiler-stage migration and
   code-replacement compatibility. The compiler-private graph must never be
   frozen as an authored project graph.
6. Write the end-to-end feature tests after these semantics and interfaces are
   complete, then refactor shared paths and ask the owner to run verification.

#### Feedback coverage friction: pending clarification

Observed source constraint, 2026-11-15:
`populate_background_topology()` adds `input_union` and authored forward
coverage to the forward dependency graph. `BackgroundCoverageState` records
coverage over global indices. A Tick/ephemeral output feeding its own
Sequential input through `detach()` can therefore require coverage from prior
Tick execution. The existing background DAG rejects the resulting recursive
forward-coverage dependency. Adding coverage history to delayed signal storage
would be a further representation and realtime-cost decision, not an automatic
consequence of reusing the background planner.

The owner clarified that Coverage ports cannot participate in feedback. The
existing normative documents already prohibit Region/Coverage random-access
inputs in their own semantic SCC. A follow-up clarification is pending because
this concrete example contains only Sequential inputs and Tick outputs; its
sparse output semantics come from `TickReplayMode::input_union`, not from an
`InputAccess::coverage` or Coverage-production declaration. Do not silently
extend the prohibition, ignore output coverage, or introduce a background
fixed-point scheduler while this distinction is unsettled.

#### Feedback coverage decision refined: prove fullness before validation

Owner agreement, 2026-11-15, after discussing the distinction between
Sequential access and upstream coverage: do not reject an SCC merely because
it contains `input_union` declarations. First prove fully covered outputs from
actual port routes and propagate that proof through generated union rules.
Ordinary live Tick outputs with no authored coverage callback, generated
`full` outputs, and Sequential module-input resolution stages provide fully
covered signals. An independently fully covered external contribution can
also prove all generated union outputs of an SCC full, even when every node
in that SCC declares `input_union`.

Sequential input resolution alone does not turn a leaf input's original
upstream coverage into full coverage. Module-input resolution does so by the
separate explicit owner decision above. Channel composition must deliver a
fully covered contribution, and authored forward callbacks retain authority
over their outputs; their behavior cannot be inferred from a union rule.

Reject only the unresolved recursive sparse-coverage dependencies remaining
after that proof. Do not add delayed coverage history or a background
fixed-point scheduler. This refines the preceding pending-friction record;
it does not ban ordinary fully covered Sequential/Tick feedback.

Coverage proof and contextual value replay remain distinct. A full-coverage
proof does not make a cycle of ephemeral value dependencies replayable.
`derive_contextual_replayability()` must still reject those replay cycles
unless a selected persisted boundary terminates the dependency traversal.

#### Live selection and mixed-input source checkpoint (unfinished)

Source work, 2026-11-15:

- `prove_full_live_tick_coverage()` now uses a dependency work list over
  normalized continuous projections and event contributions. Proven full
  generated union nodes no longer enter live sparse-coverage tracking solely
  because of their `input_union` declaration. Contextual value-replay checks
  still run independently. The current proof starts from compiler-known Tick
  production facts; it does not yet exploit a Tock callback that only proves
  its output full during runtime forward propagation.
- `BackgroundPortPlan` and `BackgroundNodePlan` retain the live-full proof.
  `BackgroundPropagationWorkspace::prepare_live_tick()` now derives a separate
  live selection from the recording transaction. Tock output Regions are fixed
  to that selected transaction; full current Tick outputs are fixed to full
  coverage. Sequential reads use live coverage, while Region/Coverage reads
  keep the recorded selection. Both domains reuse the existing F callbacks,
  scheduler and routing algebra.
- `LiveTickCoverageState` owns reusable generation-local live propagation
  state. `BackgroundEvaluationTransaction` prepares it before publication and
  commits it with the recording candidate; `TickMaterializationSnapshot`
  receives the live selection. No per-sample validity storage was added.
- Partitioning a mixed continuous connection now preserves the complete
  original projection's coverage prerequisites. Its realtime contribution
  cannot remain present just because its background sources were split into
  another delivery path. Sparse mixed inputs combine only contributions that
  exist, rather than adding an absent contribution's neutral value.
- Tiled module event targets are now expanded into their corresponding
  internal input-resolution stages. This still needs source audit and tests.

No builds or tests were run. `git diff --check` passes. The feature remains
unfinished and the working tree is not ready for verification.

#### Activation/publication friction: owner decision pending

Observed source constraint, 2026-11-15:
`GraphExecutor::activate_pending()` currently installs a generation's empty
initial `RealtimePersistedState` before the background actor prepares its
coverage and materialized input views. `TickInvocationWorkspace::bind()` can
therefore expose neutral-filled storage when required covered data is not
materialized. That must not be interpreted as absence or trigger manual
fallback under the agreed Sequential contract.

The current executor exposes `set_tick_materialization_demand(IndexCoverage)`
but a direct search of current `src/`, `vscode/`, and `tests/` found no external
callers. Automatic production playback-demand scheduling is not wired. The
published Front 4 contract says its caller chooses the future extent; it does
not supply the missing caller or an initial activation policy.

The background migration currently commits after the realtime cutover closes
and drains the predecessor's producer queues. Requiring a ready successor
snapshot before activation therefore reaches this ownership and publication
sequence, not only the fallback expression or a missing null check. Avoid
silently reusing a stale snapshot, treating unavailable data as absence,
publishing a future semantic data root into the active predecessor's domain,
or waiting on the audio thread.

The owner has been asked whether a new graph should keep the predecessor
playing until its first playback window is prepared (recommended), or activate
immediately and report playback underruns until preparation completes. Both
choices must preserve the ordered accepted transition chain. Playback underrun
remains separate from fallback on known absence. Do not encode a new
activation/publication strategy before this decision is settled.

#### Activation decision: prepare the successor before audio cutover

Owner decision, 2026-11-15: retain the predecessor's live execution until the
successor's first playback window and coherent coverage/data selection are
ready. The first graph remains pending until that preparation succeeds. Audio
activation adopts the prepared selection without allocation or waiting.
Unavailable data never becomes an absent contribution or manual fallback.

The accepted graph-transition chain stays ordered. Preparation and publication
must respect the canonical persisted-data authority and the predecessor's
still-running producer queues; an initial snapshot must not be labelled with a
newer data version than the data against which it was prepared. This resolves
the preceding activation-policy question. The handoff implementation remains
unfinished.

#### Private preparation and unchanged-output reuse checkpoint (unfinished)

Source work, 2026-11-15, following the owner's prepare-before-activation and
page-reuse decisions:

- Successor preparation now returns privately owned persisted-data and
  materialization snapshots. It does not publish the successor into the
  predecessor's canonical data domain before audio cutover. The earlier draft
  that published early and drained predecessor recordings separately was
  discarded during ownership review.
- `PersistedDataStore::Candidate::reuse_output()` shares an unchanged output's
  immutable state, Region trees and payload storage. Transactions compare
  selected recording output identities to invalidate changed dependencies.
  They retain unchanged Tock results and prepared materialized Regions.
- `RealtimePersistedState` can own the matched prepared snapshot roots.
  `GraphExecutor::activate_pending()` waits for a prepared first window and
  transfers its selection at a block boundary. Root reclamation remains on
  the background worker. The accepted transition chain remains ordered.
- Candidate revisions are allocated uniquely even for unpublished private
  preparations. Two preparations from the same canonical base must not have
  equal versions with different contents. Skipped revisions are intentional.
- The background worker requests a default eight-block playback reserve unless
  its caller selects an explicit extent. This is an implementation default,
  not a proven scheduling budget. Ongoing playback underrun handling remains
  incomplete; initial readiness must not be mistaken for that guarantee.
- Added source regressions cover output-storage reuse without rolling back
  unrelated recordings, unique unpublished snapshot versions, private
  preparation without canonical publication, and predecessor playback while
  successor preparation is blocked. None have been run yet.

This remains one unfinished Sequential-resolution/background-publication
feature. Event resolution bounds, internal-stage specialization, runtime
coverage/discovery and migration checks, ongoing unavailable-data handling,
and end-to-end tests still require work. No builds or tests were run; compiler
success is not established. The next connection is to finish the prepared
selection's availability checks before completing the planner/runtime audit.

#### Prepared selection and module resolution checkpoint (unfinished)

Source work, 2026-11-15:

- The owner selected silence-and-report for an active graph that outruns
  background preparation. `GraphExecutor::tick_block()` now checks the prepared
  playback extent before invoking any node and counts skipped blocks. The
  existing non-audio runtime monitor reports new underruns at a slow rate.
  Known prepared gaps still reach ordinary Sequential fallback.
- Internal event resolution output bounds now derive from normalized ingress
  routes, including nested stages and fan-in. Resolution-only event routing
  cycles have no authored finite producer bound and are rejected. Ordinary
  cycles through authored event producers retain their existing sizing rules.
- Eligible continuous module-input stages now emit their copy directly in a
  region specialization. They no longer require package bitcode or force an
  otherwise eligible region onto native callback interpretation.
- Background migration identifies a resolution stage by its actual public
  `StableInputPortId`. Authored output/node identities remain distinct; no
  synthetic concrete child index is invented. Unchanged stages and downstream
  delivery descriptions can therefore retain semantic coverage across bundle
  index changes. Changed fallback values still invalidate affected results.
- Code-only replacements retain the structural generation's background
  workspace. They are now rejected when their background fallback or coverage
  inventory differs; that case needs the existing ordered graph transition.
  Consequently a newly settled control that changes background replay's
  semantic values may require a transition even if storage extents match.
  Avoid replacing that correctness check with stale background metadata.
- Fixed a draft ready-frontier defect that could union individually ready
  operands after an incomplete projection, and a draft double contribution
  when manual fallback and explicit participation were both enabled.
- Sparse event output filtering now examines only events appended by the
  current callback. It preserves inherited history and in-flight events.
- Added unrun regressions for nested event capacity, direct intrinsic LLVM
  emission, public-input identity across bundle moves, background-sensitive
  code replacement, playback underrun versus known gaps, and stored manual
  fallback before module fan-out/replay.

No builds or tests were run. `git diff --check` passes. This checkpoint records
source progress, not a complete feature or successful compilation. Remaining
work includes the end-to-end live coverage and replay tests, the coverage
planning/discovery audit, and verification of the complete change before the
later automation semantics/UI work resumes.

#### Sequential-resolution source pass ready for owner verification

Source checkpoint, 2026-11-15. The connections left open by the preceding
checkpoints are now implemented in source. This is ready for a Release build
and CTest pass; it is not a claim of successful compilation or passing tests.
No builds, tests, npm checks, or typecheck were run by the agent.

The final source audit checked live versus recorded discovery selection,
private/canonical snapshot ownership, preparation/cutover ordering, module-stage
identity and intrinsic specialization, event append/history filtering, and
background-sensitive code replacement. No intentionally incomplete source path
remains in this Sequential-resolution pass. The eight-block automatic reserve
remains an unmeasured default; the accepted underrun policy handles a missed
background deadline without fabricating input values.

Regressions now additionally exercise:

- the same configured manual values through live Tick and background replay,
  with nonzero neutral values, explicit participation, and a public Sequential
  module input resolved before its child;
- a generated `input_union` feedback component that becomes full only after a
  ready Tock source publishes full coverage;
- rejection when the recursive coverage dependencies remain unresolved;
- rejection of the false proof obtained by treating one ready operand of a
  continuous projection as an independent contribution; and
- reuse of public-input semantic identity after generation-local bundle indices
  move, while a changed fallback still seeds reconciliation.

`git diff --check` passes. All five package-source literal fragments remain
below their existing 60 KiB bounds. The owner's `projects/simple_sine/` edits
and Git index were left untouched.

After verification and any resulting fixes, resume step 3's automation feature:
`CurvePointEvent`, `empty`/`hold`/automatic Bézier segments, content-derived
coverage, messages, and the matching presentation. Those behaviors are not
implemented by this prerequisite and must not be marked complete because the
Sequential-resolution tests pass.

#### First owner verification: boundary guard and fixture corrections

Owner run, 2026-11-15: 1023 discovered tests, seven executed failures and 179
fixture-dependent tests not run. The shared package and public-input access
regression both hit the retained Sequential-only guard in
`GraphBuilderPublicPorts::add_continuous_input()`. Removed that guard to honor
the accepted explicit public Sequential/Region/Coverage declarations.

Three persistence regressions assumed contiguous published revision numbers.
They now compare the actual committed snapshot identity and data, retaining
checks that failed/incomplete work publishes nothing. Unpublished candidates
still consume unique revisions. Two hand-built storage analyses now explicitly
supply their ordinary Tick producers' full-coverage facts; unknown source
coverage remains an error in the real planner. Fully covered composition
sources no longer retain redundant runtime coverage predicates.

These corrections are unverified source changes. No builds or tests were run
by the agent; `git diff --check` passes. The shared fixture must build before
the skipped GraphJit runtime regressions can supply verification evidence.

#### Second owner verification: fixes and settled module-knob friction

Owner run, 2026-11-15: 1023 tests, 12 failed. Source fixes, unverified (no build
or test run by the agent):

- Four compile failures, "sample port planning left an unresolved primitive
  port": the final binding validation still required a slot-fed input without
  history to read its slot directly. Coverage resolution now gives such an input
  a private timeline whenever its source can be sparse, and the root already
  stages it there. The validation accepts that timeline.
- "Sample input timeline references an invalid buffer" (automation window): the
  coverage-resolution loop requested a timeline before a realtime-only
  connection's binding received its layout and history, so the request recorded
  the default stereo layout. It now takes both from the connection first.
- "Sample target channel has more than one source" (summed module boundaries):
  `resolve_module_inputs()` rewrote each authored connection into a public input
  so that it targeted the stage. Several connections into one public input then
  reached target normalization as separate whole-port routes. Authored
  connections now keep their public-input targets. Each stage instead reads the
  boundary's internal side of its public input, so structural routing sums the
  contributions with their conversions and detaches, as at any module boundary.
  A disconnected public input leaves its stage input disconnected, and a
  partially connected one connects only its connected channels.
- Three playback aborts on unknown coverage: the `TickPlayback` test harness
  published materialized windows without output coverage. It now publishes the
  coverage those windows imply for each upstream output, as a transaction would.
- Wrong live values with a nonzero neutral value
  (`ReplayResolvesStoredManualFallbackBeforeModuleFanout`): the fixture declared
  `neutral_value = 0.125` with additive combination. The authored contract
  requires the neutral value to be the combiner's identity, and Sequential
  fan-in reduces from it. Live slot playback did so (0.125 + 4); replay assigned
  the single contribution (4). An agent change that made the slot assign its
  first contribution broke the two `TickInvocationWorkspace` regressions that
  document the neutral-seeded reduction, so it was reverted. The fixture now
  pairs its nonzero neutral with `a + b - 0.125`, whose identity it is; the
  expected explicit-participation values follow that combiner.

Friction observed, awaiting an owner decision. Two module manual-control tests
assert two control bindings, one per internal fan-out leaf. Under the
accepted resolution stage, one binding at the stage is the correct count. The
second test, `UniformSettledModuleManualInputKeepsImmutableInputBinding`, also
protects a realtime-cost property. A settled knob on a disconnected module
input used to bind each internal leaf's history input (history 4 in the
fixture) to an immutable constant. Each leaf now reads the stage's output, which
needs a ring and a per-block copy. Restoring the old cost needs the stage folded
into a constant producer when its resolved value is a settled constant. The
dynamic variant must run the stage, so the two variants' NodeStorage layouts
would differ. That contradicts `ModuleManualControlPreservesConversionFanoutAndDetachAcrossCodeActivation`,
which asserts equal layouts so that knob activation can be a code-only
replacement.

#### Settled module knobs: immutable consumer bindings, shared stage storage

Owner decision, 2026-11-15, revised the same day. The agent first framed the
friction above as a choice between per-block cost and layout compatibility.
The owner chose folding the stage into a constant, which made grabbing a knob
a storage-migrating transition. Asked whether a layout change was really
needed, the agent rechecked the pre-stage path. It never paid that price: settled
code bound an immutable constant to each leaf but declared the same input
timeline as its dynamic replacement, and kept that shadow timeline current only
when history is retained. The earlier framing was wrong on both counts, and the
folding (skipping the stage) was reverted unbuilt.

Selected design: keep the resolution stage in both variants. In the settled
variant, a realtime consumer whose connection identity-maps the output of a stage
with a disconnected input and a settled (or absent) knob binds the resulting
constant immutably. The connection keeps its planned storage, and the stage keeps
running. Its single output, which every consumer aliases when the knob is live,
therefore stays current for consumers that retain history. The variants keep
equal NodeStorage layouts, so grabbing the knob remains a code-only activation.
The knob is resolved once per block at the stage instead of once per leaf.

Source change, unverified: `plan_sample_ports()` in `lowering_plan.cpp` binds such consumers as
constant inputs before storage planning and skips their connection bindings.
`UniformSettledModuleManualInputKeepsImmutableInputBinding` now expects one
knob binding, at the stage, and immutable retained access on both leaves'
history inputs. `ModuleManualControlPreservesConversionFanoutAndDetachAcrossCodeActivation`
expects one binding and keeps its layout-equality checks.

Not yet implemented: skipping the stage's per-block copy when no consumer
retains history. Composed or converted consumers (for example a tile with a
constant zero) and detached consumers keep aliasing the stage output.


#### Owner verification and unstaged-fix review

The latest owner-run `build-release/Testing/Temporary/LastTest-verbatim.log`
reports 1023 tests passed, zero failures, in 84.87 seconds. This verifies the
source corrections recorded above, including the module-input routing and
settled-consumer constant bindings. No build or test was run by the reviewing
agent.

Review scope: the unstaged corrections relative to the staged implementation,
with targeted reads of structural sample routing, Sequential input binding and
composition, and the affected test fixtures. The review found no additional
correction needed in that scope. The Git index and the owner's project files
were left untouched. This is not a claim that the full background-evaluation
implementation has received an exhaustive audit.

The prerequisite verification is now green. The next implementation remains
step 3's automation feature: `CurvePointEvent`, content-derived sparse coverage,
`empty`/`hold`/automatic Bézier segments, replacement messages, and the matching
presentation. Skipping unnecessary resolution-stage copies remains a separate
performance follow-up; it is not a prerequisite for that feature.


#### Active implementation order after the output-capability revision

Accepted sequencing clarification: the output-capability documentation dated
2026-11-16 supersedes the prerequisite order in the earlier verification note.
The owner's 1023-test passing run remains the verified baseline for the existing
Sequential-resolution implementation, not verification of the revised output
architecture.

1. Implement the complete computed-output prerequisite across authored ports,
   reflection/serialization, connection validation, recording, background
   planning/evaluation and callback views. Replace `TickOutputConfig` with
   `SequentialOutputConfig` and `OutputRetention` with
   `OutputRealization::{computed, buffered}` without compatibility aliases.
   Separate callback domain from output shape. Preserve full logical Region
   metadata while computing and allocating only demanded windows, and prepare
   those windows before Sequential Tick consumers execute.
2. Complete Stage 3 automation using Tock/computed production: `CurvePointEvent`,
   exact sparse segment coverage, empty/hold/automatic Bézier interpolation,
   agreed equal-position semantics, replacement messages, serialization and the
   Vue presentation. Sequential destinations retain their neutral/manual
   fallback in known gaps. Do not extend Tick `input_union` replay to implement
   the curve.
3. Generalize buffered storage for recording and Tock/buffered results under one
   versioned-page contract, including sparse mappings, pin-safe backing reuse,
   VMA accounting and resource limits. The computed-output milestone can use
   ordinary bounded allocations; it must not create another paging engine or
   wait for the general virtual-memory migration.
4. Resume complete-buffer materialization nodes, multi-type event ports,
   optional event projections and later observation/UI work unless a concrete
   dependency requires an earlier item.

Current active feature: item 1, the computed-output prerequisite. Implementation
starts by tracing output declarations and background selection/storage together,
so a renamed API does not preserve whole-Region evaluation implicitly. New
verification must cover bounded work within huge logical Regions, unchanged
Region identities, sparse gaps, demand-order independence, connection capability
rejection, preparation-before-activation and existing buffered recordings.


#### Output-domain correction during the computed-output migration

The owner corrected the over-broad shape symmetry in the initial documentation
revision. Tick permits only Sequential outputs; Tock permits only Region or
Coverage outputs. Both share computed/buffered realization. Tock data prepared
for a Sequential consumer retain their authored Region/Coverage shape. The
incorrect passages are preserved in
[historical documentation](./historical/output_shape_symmetry_before_correction.md).

This replaces the active implementation note's statement that Tock could produce
Sequential data. The implementation order is unchanged: complete the computed
Region/Coverage demand-window path, then automation, then shared buffered-page
optimization. The output API and demand-window migration are still in progress;
the 1023-test passing run predates these source edits. No new build or test run
has verified the migration, and the working tree is not yet ready for one.

Implementation checkpoint after the output-domain correction (unfinished):

- Output API identifiers, value-extension metadata/options, configured-graph
  serialization and existing source/test declarations have begun migrating to
  Sequential/computed/buffered naming. There are no old-name compatibility
  aliases. Tick/Tock output-shape restrictions are restored in authored traits
  and GraphJit, with separate static rejection cases for the three forbidden
  combinations. These tests have not been run.
- `ContinuousRegionStorageView` has an initial prepared-window representation;
  `TransactionSampleStorage` allocates intersections with demand instead of
  entire logical Regions. Generic materialization, snapshot freezing, shifted
  bindings, event bounds and storage validation still need matching changes.
- Initial demand-widening removal and `InputContentAccess` declaration/archive
  plumbing are incomplete. Connection capability validation, callback-visible
  logical coverage versus prepared windows, and buffered publication still need
  to be reconciled before this is a complete execution path.
- Existing project source declarations using `OutputProduction::tick` still
  need migration. The earlier replay/live-coverage coupling and Tick-based
  automation also remain migration work.

The next bottom-up connection is the content-view/storage path: preserve whole
logical Region metadata and duplicates while validating and freezing only the
selected prepared windows. Continue through connection capability checks and
execution before requesting a build. The working tree may currently have
compiler errors owned by this migration; it is not a new verified baseline.


#### Computed-output source pass and complete-buffer Tick decision

This checkpoint supersedes the unfinished source checklist immediately above.
The owner chose to initially require **complete-buffer Tick Region/Coverage
inputs**, rather than adding a callback for future playback requests. The rule
is enforced in authored traits and GraphJit. Tock inputs may be windowed.
Computed-to-complete-buffer connections are rejected; a buffered source or an
explicit bounded materialization node is required. Outputless Tick sinks issue
complete input demands and wait for preparation before activation.

The current source pass includes:

- Sequential output configs and computed/buffered realization throughout the
  authored API, value-extension metadata, reflection, configured-graph archive,
  node declarations and plans. Tick and Tock enforce the disjoint output matrix.
- Demand-scoped continuous and event storage, conversion work, and callback
  views. Logical Region ranges, duplicate entries and overlap remain intact;
  prepared content windows are tracked separately. Extra cached content is
  hidden from a windowed invocation, including discovery views.
- Partial buffered publication with separate logical coverage and valid content
  coverage. Unchanged continuous windows share independent immutable payload
  owners; obsolete version lists need not remain alive. Complete-buffer reads
  gather cached fragments only when their full contents are available.
- Tick snapshots retain bounded physical owners and whole logical metadata;
  identity routes can share prepared payloads. The executor continues to prepare
  before activation and treats unavailable covered playback as an underrun.
- Live Sequential output coverage no longer depends on `input_union` replay.
  Intrinsic replay remains a separate permission; a replayable node's own
  outputs must be computed Sequential outputs. The existing linear automation
  interpreter now executes as Tock/Coverage/computed, while richer segment
  semantics remain item 2 of the active order.
- Regression cases for forbidden output domains, windowed Tick inputs,
  computed-to-complete-buffer rejection, huge logical Regions with tiny content
  windows, conversion work bounded by demand, duplicate and cache-independent
  callback views, partial sample/event validity, snapshot sharing/reclamation,
  and complete preparation for an outputless Tick sink.

The source pass is ready for the owner's build and test run. **No build or tests
have been run for these changes.** `git diff --check` and fixture-literal size
checks pass. The earlier 1,023-test result remains the last verified baseline.
The subsequent automation and general page-backed buffered-storage work are
still outstanding; neither is claimed complete here.


#### First computed-output test run and repair pass

The owner built the source pass above and ran 1,032 tests: 25 failed. This
supersedes the untested checkpoint above as an observation of that source pass;
it does not establish a new successful baseline.

The current repair pass addresses two runtime defects:

- `BackgroundPropagationWorkspace::require_output` now checks buffered content
  availability when reverse callbacks discover new requirements. These
  dependencies could previously arrive after the initial demand-expansion pass
  and never activate their producer. Changed cached contents also count as
  missing for a required window. Discovery scopes retain their own bounded
  demand-expansion callback.
- `TickMaterializationSnapshot::carry_forward` preserves complete Region order
  and duplicate occurrences, and maps retained physical windows to the current
  logical Region list when that list changes order. Partial replacement shares
  unchanged payload owners without clipping logical Region metadata.

The remaining failing fixtures have been aligned with the migrated contracts:
Tock curve invocation, bounded output writes with complete logical metadata,
computed versus buffered source storage, dense Sequential replay, and mapped
immutable payload access. GDB on the existing FFT replay test also identified
an unconstrained fixture output size that prevented the intended rejection
check from being reached. Regression cases cover delayed buffered dependency
activation, partial sample replacement, and reordered logical Regions.

**These repairs have not been built or tested.** The next step is the owner's
build and complete test run. The richer automation feature remains next after
this execution path has a successful baseline.


The next owner run built successfully and passed 1,032 of 1,034 tests. Two
failures remained. The current follow-up changes are not yet built or tested.

`BackgroundStorageRealization::make_tick_materialization_snapshot` now receives
explicit requirements for each input. Shared storage availability no longer
selects an unrelated Tick input. This prevents a narrow producer request from
trying to freeze an unrequested complete-buffer event input. Selected Sequential
contents also stay within their requested windows.

The buffered-publication test now distinguishes a logical coverage change from
a request for new contents. It tests metadata-only invalidation followed by a
bounded refill. The snapshot test also checks that undemanded inputs stay absent
and that sample and event contents obey the selected input windows.


#### Owner-verified computed-output checkpoint

The owner reports that all tests pass after the repairs above. The owner also
reports that the application functions in initial use. These results supersede
the untested status of the repair passes above. The agent did not independently
run the build or tests.

The minimum computed-output execution path now has a passing baseline. The next
feature is automation end to end: `CurvePointEvent`, sparse `empty` segments,
`hold` segments, automatic Bézier interpolation, stable equal-position ordering,
replacement messages, serialization, and the Vue presentation. General buffered
storage optimization remains after this automation feature.


#### Active feature: sparse automatic automation

Start from the owner-verified computed-output baseline above. Implement the
complete automation feature in this order:

1. Add `CurvePointEvent` and its event type through runtime validation and
   transport. Preserve `SampleEvent` for its existing scalar-event meaning.
2. Replace the linear interpreter with a Tock/computed curve interpreter. Use
   point-input discovery for exact segment coverage and bounded output windows.
   Use spacing-aware clamped tangents, break tangents at jumps and beside
   hold/empty segments, and blend exact linear interpolation at tension 0 with
   the automatic cubic at tension 1.
3. Extend construction values, replacement messages, client bindings and the
   Vue editor together. Preserve point IDs, gesture history and accepted reads.
   Display actual gaps and the same interpolation convention as the backend.
4. Add tests for sparse coverage, cyclic boundaries, equal-index jumps,
   interpolation, upstream transformations, fallback and editing. Refactor after
   these paths connect. The owner runs compilation and tests.

The richer automation feature is in progress. The passing baseline does not yet
include these changes. General buffered-storage optimization remains later.


#### Sparse automation source pass and current verification

The source pass now connects `CurvePointEvent`, Tock point discovery, exact
empty/hold/Bézier coverage, and bounded interpolation. The interpreter uses
weighted harmonic PCHIP tangents and breaks tangents at extrema, jumps, and
boundaries beside hold/empty segments. Tension 0 remains exactly linear.

`PointSource` orders equal sample indices by persistent point ID. An empty
source publishes a valid one-sample event Region containing zero events.
`Curve` derives empty coverage from those contents. The source's storage Region
never defines the interpreter's active intervals.

The editable module retains `iv.builtin.automation.curve`. Its interpreter leaf
is now `iv.builtin.automation.interpreter`, replacing the misleading linear-only
name. Point construction, `AddPoint`, `MovePoint`, `RemovePoint`, `SetSegment`,
client bindings, and the Vue presentation carry segment policy and tension.
The editor draws separate paths across gaps and uses the backend's interpolation
convention. Point gestures retain the existing accepted-read and history path.
The old builtin guide is preserved under `docs/historical/`.

Regression additions cover clamped interpolation, exact linear tension,
equal-position jumps, empty point groups, transformed upstream events,
Sequential manual fallback, and frontend segment editing. Fixture literal sizes
and `git diff --check` pass. A syntax-only compiler-plugin invocation produced
neutral event metadata for refreshing the client bindings. No full build or test
suite was run by the agent for this feature.

The owner reported a client TypeScript error because compiler enum schemas
include `signed`, which `NodeEventValueSchema` omitted. The field and a signed/
unsigned enum-binding regression are now present. This fix awaits the owner's
next npm test run.

The owner also reported a server restart when connecting automation to an
oscillator frequency input. The exit log reports SIGTERM. No server core dump
for this incident is visible through `coredumpctl`; the extension sends SIGTERM
on RPC connection loss, so the exit signal alone does not identify the initial
failure. The preceding connection-loss log is requested. Diagnosing this restart
and completing owner verification remain part of this automation feature.


#### Automation connection restart: captured cause and repair

An isolated project under `build-release/automation-socket-repro` reproduced the
owner's connection failure with the existing server binary. The automation-to-
frequency request succeeded, then the server exited with code 1 after reporting
background execution failure. GDB captured the underlying exception:
`Tick materialization retention clips a logical Region`. The extension's
SIGTERM followed the RPC socket closure and obscured that original exit.

The missing distinction was in `TickMaterializationRetention`. Its previous
record contained a port, retained positions and a logical Region list, but no
input access kind. `carry_forward()` applied a complete-Region rule to partial
Sequential playback windows. The repair carries `BackgroundPortPlan::input_access`
into the retention request. Sequential samples and events retain bounded
subwindows through shared immutable owners. Region/Coverage retention keeps its
logical order and identity rules. The coordinator no longer constructs a
logical Region list for Sequential retention. This adds no sample copy or work
to the realtime callback.

New regressions cover partial sample/event retention, preserved known-empty
event coverage, and automatic rolling playback through a module input. The
background executor also logs its first underlying failure before publishing
the sticky failure flag, so a controlled shutdown does not hide the cause.

The original binary reproduced the defect. These source repairs are not yet
built or tested. The owner must rebuild and run the suites before this automation
feature has a new verified baseline. The TypeScript enum-schema fix also awaits
that verification. No Git staging or project edits were performed by the agent.


#### Final source review, 2026-11-16

The owner ran 204 frontend tests. TypeScript compilation succeeded and 203 tests
passed. The remaining failure exposed a viewport-edge error in
`automationWindow()`: the single-point constant case limited its logical
coverage to the viewport. It now keeps full-circle coverage while clipping only
the drawn path. The regression also checks a viewport with no visible control
point and samples both of its edges.

The owner also found a C++ test compilation error: `find_port` belonged to a
lambda in another test. The transformed-curve test now defines its lookup in the
correct scope. The message regression also checks that moving a point preserves
its segment policy and tension.

The planned source changes for sparse automatic automation are in place. The
backend and frontend suites, typecheck, and app verification remain pending after
these repairs. No additional design decision is currently blocking this pass.
Do not close the automation stage until the owner verifies the updated source.
The next architectural work remains the separately planned buffered-storage
work, after the automation baseline is verified.


#### Shared fixture repair and isolated connection verification

The next owner run reported 187 unsuccessful tests out of 1,040. The log contains
three actual failures and 184 tests skipped because their shared package fixture
failed. The fixture used a nonexistent `GraphBuilder::module()` call and an
undeducible `SequentialInputConfig`. It now uses the existing `graph.subgraph()`
path, `SubgraphBuilder`, and `input<"frequency", Sample>()` with explicit Sequential
access. The two independent failures were stale assertions: the builtin module
now has four message handlers, and the zero-point source has a valid empty event
group rather than an empty Region range.

The complete shared fixture, extracted in its authored concatenation order, now
passes a syntax-only compiler check. Fixture literal sizes remain below 60 KiB.
The owner run also passed both new Sequential partial-retention regressions.

The existing rebuilt server was checked with the isolated automation-to-frequency
reproduction. The mutation was accepted, the graph advanced to generation 2,
and five subsequent scope requests succeeded while the server remained alive.
The earlier background-retention shutdown did not recur in this bounded check.
This does not establish a successful full-suite baseline. Rebuilding the corrected
test fixtures and rerunning the C++ and frontend checks remain the next step.


#### Owner verification, 2026-11-16

The owner reports that the latest corrections work and that the automation output
connects successfully in the application. This supersedes the pending verification
notes above. The agent has not independently rerun the suites. Playback controls
are still absent, so this is not verification of playhead or play/pause behavior.
The next client-stage dependency is the `Node::State` lifecycle cleanup recorded
in [Vue Client Implementation Stages](vue_client_implementation_stages.md).
Shared versioned buffered pages remain later architectural work.
