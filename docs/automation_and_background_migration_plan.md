# Automation And Background Planning Migration

_Implementation coordination, 2026-11-15. This is an implementation order, not
another normative contract. Current source progress is recorded below._

The governing decision is commit `d2e1d0ec`, especially
[Cyclic Automation, Event Projections, And Suspendable Coverage Propagation](automation_and_suspendable_coverage_direction.md).
The implementation baseline is `6826b784` (`automation clip v1`). The worktree
was clean when this comparison began. The earlier Stage 3 checkpoints describe
that baseline, not completion of this migration.

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
