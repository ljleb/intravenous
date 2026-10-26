# Runtime Performance Roadmap

_Status: implementation checkpoint. This note records known runtime scaling work
without expanding the normative execution-model documents. Semantic contracts in the
linked design documents continue to govern implementation choices._

The actor split, independently pinned producer-queue prefixes, generation draining,
and ownership model are not presently the main scaling concern. Persisted data has now
moved from a globally linear page index to structurally shared per-output range trees.
The next cluster of costs comes from per-value work where the data model already
exposes region, chunk, ring, or contiguous spans.

The governing performance invariant is:

> Realtime work must not grow with retained recording duration, and publishing a
> successor must scale with changed records and changed output metadata rather than with
> the complete persisted history.

## Ordered implementation work

1. **In progress in the active circular-index/store migration:** replace the persisted
   snapshot representation as one coherent unit:
   - resolve stable and generation-local output identities off realtime into compact,
     non-recycled handles; snapshots and realtime/background data access carry only
     those handles, never semantic identity variants or strings;
   - index each store-local handle slot through an immutable path-copy radix directory,
     so output selection performs a bounded sequence of direct digits rather than an
     output-count-dependent comparison search;
   - give each output a persistent/path-copy balanced range index keyed in explicit
     signed `GlobalIndex` order;
   - structurally share unchanged output directories, range subtrees, and record payloads;
   - retain per-output `IndexCoverage` in its immutable path-copied interval tree so
     one write/erase does not copy every historical coverage run;
   - make lookup, mutation, successor publication, and retired-root destruction
     independent of total retained recording duration except for nodes actually changed
     or made unreachable. The output directory, record trees, region/chunk payloads,
     span playback, and persistent `IndexCoverage` metadata are implemented in the
     active working tree; compiler/test verification of the coherent migration remains
     before this item is complete.
2. Complete semantic transition planning. Cutover work must be bounded, not necessarily
   copy-free. Preserve inherited state by reading frozen predecessor storage directly
   where appropriate, or by using compact transition-only carries/materializations.
   Small bounded migration copies are valid; synchronous copying of whole histories or
   unrelated raw regions is not. Prepared generations now own the cold semantic match
   of surviving sample/event port-state identities, including exact inherited timeline
   intervals and growing/shrinking extents; remaining physical transition realizations
   and bounded transition expiry still need implementation from that plan. Each inherited
   state is now also classified as already represented, transferable into one
   successor steady-storage view, requiring transition-only storage, or
   unavailable from the predecessor; shared successor views cannot be selected
   for transfers of distinct semantic states. Transferable states now produce
   concrete sample/event descriptors containing predecessor storage and
   conversion facts, the private successor storage view, and the exact inherited
   interval. Activation now receives the exact cutover sample and executes
   direct sample-ring, compact-sample-carry, and event-ring transfers at that
   boundary. Materialized sample histories and converted/indexed event
   representations remain transition-only until their bounded reconstruction is
   implemented.

   **Implementation-friction review (2026-10-18; unresolved):** the required fact is
   bounded inherited semantic state, not necessarily two complete compiled
   realizations. A separate transition/steady pair remains the documented way to keep
   the final realization free of temporary storage and work, but it has not yet been
   justified against either one successor realization with an expiring prepared
   overlay or deliberately private successor state. The latter removes the second
   handoff but may impose permanent copies/storage; the overlay may impose dynamic
   binding or branch cost. Complete the allocation-free direct-cutover path and one
   representative rewiring/fan-in transition before selecting among them. Record the
   measured lifetime, compile-time, steady-state and realtime-cutover costs here; do
   not add multi-realization ownership merely because the current single-realization
   types cannot express the first alternative.

   The leading overlay candidate is a stable steady-state `NodeStorage` prefix plus a
   compile-time-bounded transition-only suffix, shared by separately emitted
   transition and steady root entrypoints. This removes the second node-state
   migration: expiry only changes the selected entrypoint. Before adopting it, add an
   explicit steady-prefix boundary for nested-node state spans and quantify the dead
   suffix capacity retained by the contiguous allocation after expiry. The boundary
   and transition-only region declaration now exist in `NodeLayout`, but GraphJit does
   not yet allocate or bind a suffix. The tail must also remain discoverable as a
   semantic source when a newer revision supersedes an active transition; it must not
   acquire an ordinary persistent raw-region identity.

   Direct project-rate ring/carry cutovers now have a cold preparation stage which
   validates descriptors and storage bounds and rejects unsupported inherited-state
   forms before publication. The prepared realtime transfer performs no allocation and
   exposes no recoverable error path. `NodeStorage` lifecycle commit can still fail, so
   this does not yet make the complete activation boundary non-failing.

   **Implementation friction:** predecessor reconciliation currently begins in the
   executor only after successor lowering and allocation, too late to determine a
   transition suffix or emit a transition root. The same layout also creates the
   background storage instance, which would duplicate an appended realtime-only tail.
   A steady-only allocation extent is now implemented: background `NodeStorage`
   instances omit the transition suffix even though they retain the canonical layout.
   The predecessor must not simply come from the prior `ProjectGraph` generation,
   because pending generations can be superseded and the executor's active predecessor
   can be older. `GraphExecutor` already distinguishes `realtime_active_` from the
   replaceable `pending_generation_`. `prepare_stage()` now snapshots only the former
   into an immutable compiler-coordination context, and `stage()` rejects that context
   if realtime activated a different predecessor while specialization was in flight.
   Pending replacement does not invalidate it. Before transition-root expiry is
   connected, extend the same context token to distinguish that active transition
   realization from the later steady realization of the same logical generation. A
   finalized steady `NodeLayout` can now
   be extended through `NodeLayoutTransitionBuilder`, which preserves all prefix
   offsets and admits only a transition suffix. The representative project-rate direct-
   ring sample case can now allocate a private suffix target when the steady target is
   shared, and the prepared cutover transfer populates it. Connect these completed
   primitives to GraphJit transition binding/root emission; do not force every graph
   edit to activate or reserve all possible transition tails.

   **Representative connection implemented (2026-10-18; verification pending):**
   cold transition specialization now binds feed-forward project-rate direct `Sample`
   input histories to private suffix rings, emits a transition root which mirrors new
   producer samples into those rings, and emits the unchanged steady root beside it in
   the same module. `GraphExecutor` activates that root from a prevalidated cutover,
   exposes the suffix as the active semantic realization to a newer specialization, and
   switches to the steady entrypoint at a callback boundary after the finite history
   horizon. The staging token includes the active transition/steady phase, so expiry
   during compilation invalidates the result even when the logical predecessor graph is
   unchanged. Expiry after staging but before activation likewise discards the prepared
   generation at the quiescent boundary and requires cold respecialization; a stale
   suffix is never committed. Background storage continues to allocate only the steady
   prefix.

   This path currently rejects output history/latency, feedback producer order,
   materialized/composed and non-project-rate samples, and event transition storage.
   Specialization also repeats cold package parsing/lowering; record its measured cost
   before deciding whether a reusable host-side lowering artifact is justified. The
   application-side staging owner still has to call the now-complete
   context-to-specialization-to-stage contract when that runtime integration front is
   connected.

   Initial full-suite verification compiled and passed 529 of 530 tests. The remaining
   generated-root test used directly configured concrete nodes, which do not carry the
   virtual-node identity currently required for cross-generation matching. Its
   corrected fixture uses a registered module graph, preserves that identity across a
   copied connection rewrite, and the subsequent full-suite run passed all 530 tests.

   The next expansion reconstructs retained linear sample materializations during
   prepared cutover, groups disjoint state roles/channels of one authored port into one
   suffix timeline, and supports shared Tick sample output history/latency by mirroring
   transition writes into the ordinary producer representation before existing fanout
   and carry operations. A dedicated late mirror slot follows feedback/composition
   writes, so transition history no longer assumes producer-before-consumer schedule
   order. Verification of this expansion is intentionally deferred to the human-run
   build and suite. Composed predecessor state, non-project-rate state, and event
   transition suffixes remain open.

   A subsequent cursor audit found that both cutover ring selection and transition
   expiry still used node-visible project coordinates. The expansion now records each
   sample realization's persistent per-pace Sequential cursor. Reconstruction reads the
   predecessor and successor cursors independently, so transport seeks and resolved
   pace changes do not reinterpret ring contents. Transition lifetime is no longer a
   scalar `GlobalIndex` distance: the executor preallocates one progress record per
   affected pace, snapshots its migrated cursor at activation, and selects the steady
   root only after every required transported-value count has advanced. The realtime
   checks remain bounded and allocation-free. A pre-consumer feedback materialization
   can also feed its transition mirror in the existing ordered before-callback phase.
   Human compilation and suite verification are pending for this expansion.

   Further Front 6 work had exposed two semantic contracts rather than storage
   optimizations. Sequential event retention will keep an expected-next-position marker
   and rebase its bounded ephemeral `TimedEvent` values by the transport seek delta.
   This keeps the node-facing window globally sorted while durable persistence and
   Random Access retain authored timestamps. The marker plays the role of the continuous
   private cursor; no per-event ordinal sidecar is required. Dynamically sized continuous
   values now use an optional type-owned shape-migration operation. Absence explicitly
   means fresh initialized successor state; `FFTBlock` currently uses that default, so
   generic transition code never invents a raw byte-prefix policy.

   Human verification of the cursor/output-window expansion compiled and passed 532 of
   533 tests. The failure occurred before transition lowering because its disconnected
   producer expression had no named source declaration and therefore no stable virtual
   identity. The fixture now names that declaration and checks the two output-window
   identities before expecting a transition root; verification is pending.

   The next source-level expansion implements those two contracts. Ephemeral
   Sequential event storage now rebases live ring/carry timestamps from a persistent
   expected-position marker, while transition expiry counts processed callback extents.
   Direct, compact, converted, and indexed event cutovers merge into steady storage;
   producer-index sidecars preserve recoverability for retained fan-in. A semantic
   event state that cannot receive a direct steady transfer uses a private transition
   ring. This includes a derived event input and, after the Front 7 raw-output ownership
   change, an event output that shares its steady sequence with an input. The earlier
   output-suffix assumption is preserved in
   [`historical/front_6_event_output_transition_suffix_assumption.md`](historical/front_6_event_output_transition_suffix_assumption.md).
   Continuous cutover now evaluates flattened composed sources, and dynamic shapes use
   the optional type-owned migration hook or explicitly start fresh. Focused tests cover
   these paths, but compilation and suite execution remain with the human collaborator.

   The generation handoff now also prepares stable-output coverage mappings and commits
   them only after the predecessor's finite queue tails drain. Incompatible or vanished
   persisted destinations are retired in the successor reconciliation transaction;
   sample compatibility includes the complete selected channel set as well as its
   layout. A successor with no queued inputs still performs one reconciliation
   publication. This closes the gap where late old-generation recording could reach the
   canonical store but remain invisible to the newer realtime generation until
   unrelated future work. Verification remains with the human collaborator.

   Reconciliation now erases an incompatible inherited destination before applying any
   successor records for the same stable handle. An empty successor can execute this as
   a retirement-only transaction and publish the matching empty Tick-materialization
   root without generated background operations. The transition/steady phase observed
   by cold staging is an atomic token; mutable progress remains realtime-owned.

   One correctness boundary remains before this front can be declared complete.
   Background plans do not yet carry enough reconciliation input to distinguish
   unchanged Tock semantics from changed code, configuration, or upstream computation
   when output identity and storage shape are equal. Do not substitute graph generation
   or package-wide revision numbers: those would invalidate unrelated stable outputs and
   conflate recompilation with semantic invalidation. Either carry the explicit stable
   semantic-change set owned by the application mutation path through compilation and
   staging, or define a canonical per-computation/dependency fingerprint in the compiled
   plan. The first option matches the external invalidation-root ownership already
   specified by the coverage design; the second makes compiled generations
   self-describing. Choose that ownership and define its exact invalidation-root
   contract before completing this handoff.

   **Resolved in source (2026-10-19; human verification pending):** compiled
   generations now carry stable local semantic facts rather than a transitive
   fingerprint. Registered primitive identity, provider-definition version, reusable
   typed-configuration ownership, compiled background execution mode, and semantic
   port shape classify local-node continuity. `NodeInstances` retains a configuration
   across snapshot publication only while its complete recursive definition closure
   retains the same versions, so a package rebuild invalidates every affected use but
   an unrelated refresh preserves cache identity. Stable input ports separately carry
   normalized connection descriptions. Handoff comparison with the exact active
   predecessor emits local-node roots and exact input-change roots; existing coverage
   propagation supplies transitive invalidation. The input root uses the union of old
   and new input coverage after predecessor drain, and a sample-rate change roots the
   full background plan. This keeps ordinary small edits proportional to their affected
   graph while still handling large structured edits and broadly reused rebuilt node
   kinds without a special path.

   **Remaining lifecycle friction (2026-10-19):** realtime activation still calls
   `NodeStorage::Migration::commit()`, whose authored move/release/deferred-initialize
   callbacks can throw and whose initialize path may allocate `TockState`. Partial
   callback execution is not rollback-safe. Do not solve this with exception catching
   at the cutover boundary. Define either a separated background/Tock preparation plus
   statically non-throwing realtime `State` commit, or a real transactional lifecycle
   contract, before calling Front 6's activation path complete.

   **Resolved in source (2026-10-21; human verification pending):** lifecycle callback
   failure is now an explicitly fatal contract. The erased initialize/move/release
   function pointers are `noexcept` wrappers around arbitrary authored exceptions. An
   escaped exception takes a cold reporting path which writes the operation, node type,
   node index, active exception, and stacktrace to the server diagnostic log before
   aborting. VS Code assigns and reports that log path through `IV_LOG_FILE`; an
   operator-supplied path wins, and crash-handler installation opens the stream before
   lifecycle work. `NodeStorage::Migration::commit()` and realtime
   migration commit are `noexcept`. No `std::expected` is returned because no valid
   recovery or rollback action exists after partial lifecycle execution, and avoiding
   it also avoids a result branch on every successful cutover.

   **Front 7 storage follow-up implemented in source (2026-10-21; human verification
   pending):** the remaining Tock allocation concern is now removed from realtime
   storage and migration. `NodeStorage` owns only realtime `State`; background-only
   `TockState` has a separately indexed `TockStateLayout` and one compact
   `BackgroundExecutor`-owned sidecar allocation. Successors construct fresh Tock state on the
   background worker rather than migrating it, and generated background roots receive
   the sidecar through their call frames instead of accepting a `NodeStorage` base.
   This supersedes the earlier implementation-status notes that background execution
   used a steady-prefix `NodeStorage` instance or that realtime initialization could
   allocate Tock state; those notes remain above as provenance for the transition and
   lifecycle decisions.

   **Front 7 declaration-layout follow-up implemented in source (2026-10-21; human
   verification pending):** nested declarations and authored local/shared arrays now
   use the existing packed `NodeLayout` storage path. A top-level primitive map keeps
   nested records separate from graph primitives. Lowering resolves the map and emits
   final state offsets. No callback performs a map lookup. Shared-array dependency
   ordering and span patching remain cold initialization work.

   **Front 7 activity follow-up implemented in source (2026-10-21; human verification
   pending):** TTL is disabled unless a node instance or node type enables it. Disabled
   nodes have no activity storage, scan, or generated branch. Enabled nodes use
   registry-owned semantic equality and cleared-value operations, compact persistent
   activity state, and generated Tick-or-skip selection. Static constant inputs need no
   scan. Generated lowering now shares compact continuous and event activity facts
   across equal producer or derived-frontier windows. Per-representation emission
   epochs invalidate facts after planned writes. Continuous comparison state is shared
   only when unsliced acyclic consumers observe the same root window. A later project
   or user setting will supply the configurable default TTL. No project-wide default
   exists yet.

   **Front 7 routing friction resolved (2026-10-21):** `ConfiguredGraph` already keeps
   the authored connection segments and structural bundle information. GraphJit will
   traverse that structure once while it builds ordinary connection plans. It will not
   retain another general route graph. Each concrete source dependency can contain at
   most one detach. Repeated serial detaches are errors because both cannot be
   necessary to break cycles on the same dependency. The existing concrete-graph
   validation that rejects unnecessary detaches remains authoritative after bundle
   expansion.

   **Front 7 non-detached routing implemented in source (2026-10-21; human
   verification pending):** GraphJit now traverses subgraph boundaries and tiled
   bundles during connection analysis. Execution and lowering inventory only concrete
   primitives. Sample routes use one fused coefficient transform in realtime and
   background materialization. Event routes use one concatenated conversion program.
   Tiled sample inputs project after conversion. Tiled event inputs broadcast, and
   tiled event outputs merge. Focused connection-plan tests cover these paths.

   **Front 7 per-source detach implementation (2026-10-22; collaborator verification
   pending):** configured sample channels and event sources now own their optional
   detach settings. Structural traversal carries those settings to concrete sources
   while it fuses sample transforms and event conversion programs. GraphJit validates
   and binds each removed dependency separately. Equal settings across all event
   sources retain one shared feedback buffer. Mixed settings keep source-index data
   for ordinary branches selected from retained producer-group storage. Each detached
   branch reads the exact callback-local or source-converted representation that feeds
   the producer-group merge, and ordinary and delayed streams merge immediately before
   the consumer. The event conversion still runs once after that merge. Event fan-in formed from existing
   `EventPortRef` values preserves each expression's detach settings. Filtered runtime
   helper tests cover every sequence/ring source and target combination. A full
   GraphJit fixture covers an ordinary acyclic source combined with one detached SCC
   source. The rejected conversion-split model is
   preserved in
   [`historical/front_7_single_route_detach_split_false_start.md`](historical/front_7_single_route_detach_split_false_start.md).
   The superseded implementation that filtered detached branches out of the sorted
   producer-group sequence is preserved in
   [`historical/front_7_merged_event_detach_filter.md`](historical/front_7_merged_event_detach_filter.md).

   **Front 7 event-route representation friction recorded before resolution
   (2026-10-22):**
   `ResolvedEventBranch` already preserves a source-specific `EventConversionPlan`,
   but `RoutedEventConnection`, `EventConnectionPlan`, and
   `EventProducerGroupPlan` require one common source type and conversion for the
   complete source set. Different structural conversion chains to one event input
   consequently become two input bindings. Event output storage is also owned by an
   exact source set, so one `EventOutputPortId` cannot yet participate in two distinct
   fan-in sets. The next event-planning change must own canonical raw storage per
   output, preserve conversion per resolved source, convert to the authored fan-in
   type before merge, and apply the common connection conversion after merge.

   **Front 7 event-route representation resolved in source (2026-10-22;
   collaborator verification pending):** one `EventConnectionPlan` now retains the raw
   type and conversion for every `EventSourcePlan`. Realtime and background execution
   convert each source to the authored fan-in type, merge once in authored source order,
   and then apply the common connection conversion. Each concrete `EventOutputPortId`
   owns one reusable callback binding and optional retained raw sequence. Different
   `EventProducerGroupPlan` records can consume that same raw output. A direct retained
   single-source connection still aliases the raw canonical sequence.

   Multi-source groups use a separate fan-in representation. They do not overwrite a
   raw `EventOutputPortId` representation with events from other outputs. The
   superseded producer-home design is preserved in
   [`historical/front_7_event_fan_in_producer_home.md`](historical/front_7_event_fan_in_producer_home.md).

   Transition specialization now handles the direct retained alias when an output and
   input share one steady sequence. It starts the shared steady sequence fresh, transfers
   the two predecessor semantic states into separate transition-only rings, appends new
   callback-local events to the output ring, and materializes current delivery into the
   input ring. Focused tests cover differing structural source conversions, overlapping
   fan-in sets, conversion before merge, and this shared-state transition. Compilation
   and suite execution remain with the collaborator.

   **Front 7 detach semantics correction (2026-10-21):** `detach()` marks dependencies
   that GraphJit removes to break a cycle. It does not create a storage operation.
   Therefore, equal detach settings do not need an authored identity.
   `g.tile<stereo>(l, r).detach(1)` and
   `g.tile<stereo>(l.detach(1), r.detach(1))` must produce the same configured
   representation. A mixture of detached and undetached sample contributions needs
   per-contribution detach settings if Front 7 accepts it. Storage planning remains
   free to combine or separate retained values. Calling `detach()` on an expression
   that contains any detached contribution is an error, including when the settings
   are equal. The superseded identity proposal is
   preserved in
   [`historical/front_7_detach_identity_false_start.md`](historical/front_7_detach_identity_false_start.md).

   **Front 7 dependency-ready source status (2026-10-22; collaborator verification
   pending):** every Front 7 item that does not depend on Front 2 is implemented in
   source. `plan_declarations()` continues to reject an unowned
   `NodeLayout::Region::Kind::raw` region because no declaration record binds those
   bytes to a node field. Root boundary storage belongs to concrete system or
   communication nodes. Normalized block replayability waits for Front 2 batch
   operations. Front 7 is source-complete at this dependency boundary.

   **Front 7 verification follow-up (2026-10-22; collaborator re-run pending):** the
   first complete 572-test run passed 555 tests. A preserved event fan-in target had a
   linear transient lifetime even though `EventMergePlan::preserve_existing_target`
   extends it across repeated slices of one cyclic `ExecutionRegionPlan`; its lifetime
   now covers the complete SCC region, which prevents earlier primitives on the next
   slice from overwriting the accumulated sequence through arena reuse. Tock-only
   primitives are also excluded from realtime sample/event binding records and from
   disconnected Tick sample/event, transient-lifetime, and execution-operation
   planning; their ports use the supplied `BackgroundNodeCall` bindings. Projected
   sample-detach, migration-identity, producer-home region-count, conversion-count,
   packed-stack diagnostic, and no-op transition tests now assert the current
   representation. No extra realtime operation was added. Human compilation and
   full-suite verification remain pending.

   **Second Front 7 verification follow-up (2026-10-22; collaborator re-run pending):**
   566 of 572 tests passed. Routed sample-feedback alignment identities no longer have
   the old `:samples` suffix, so the two migration-test lookups now match the current
   identity. Mixed event detach now appends from the exact per-source representation;
   it does not keep a numeric cursor into a sorted fan-in sequence where a later SCC
   slice can insert an event before that cursor. Retained event-input realizations now
   record which output-latency requirements supply their producer-group future
   interval. A supplemental cold `EventRealtimePortStateTransfer` seeds each surviving
   future into that input storage at cutover; steady callback-local merging is
   unchanged and does not duplicate retained events. The compact-retention test
   matches each output/input realization to its own persistent region. Raw-output
   costing also includes the callback-local telemetry sequence when
   full `NodeStorage` is selected, so an unavoidable stack limit reaches the final
   packed-stack diagnostic. Collaborator compilation and full-suite verification are
   pending.

   **Front 7 full-suite verification and adversarial completion audit (2026-10-22):**
   the next collaborator run compiled and passed all 572 tests. New focused tests now
   cover four previously unexercised Front 7 contracts: rejection of two serial
   detach records revealed by subgraph-boundary expansion, per-source rejection of an
   unnecessary detach in a connection that also contains a necessary detach, and
   exact-generation migration of compiler-owned activity/TTL state. Event activity now
   also requires empty input windows to remain skippable and a later non-empty input
   window to wake the primitive. The tests do not change source semantics. Their
   collaborator build and execution remain pending.
   Front 7 is source-complete at its current dependency boundary; normalized block
   replayability remains ordered after Front 2's normalized batch operations.

   The first run with these adversarial tests passed 575 of 576 tests. The only
   failure was an incorrect assertion in
   `CompilerOwnedActivityStateMigratesAcrossCompatibleGenerations`: authored
   `ActivityProbe::State` starts fresh in the successor, while the compiler-owned
   activity region migrates independently. The successor's immediate `skip_block()`
   call demonstrated the intended migration. The corrected assertion awaits a
   collaborator rerun.

   **Adversarial completion verification (2026-10-23):** the corrected suite passed
   all 576 tests. Front 7 is closed at its Front 2 dependency boundary. Normalized
   block replay now belongs to the next Front 2 callback-normalization pass.

   **Front 2 source implementation (2026-10-23; collaborator verification pending):**
   public Tick, skip, Tock, forward, and reverse batch ranges and their scalar/batch
   normalization now exist. `NodeCompilerOperations` retains both anchors for each
   applicable operation. GraphJit imports them, classifies nodes by `NodeCodeKey`,
   package revision, and callback-facing `ResolvedPortRealization`, and records exact
   ready-frontier antichains in `SchedulePlan::ready_groups`. Realtime lowering uses
   native batches for uncomplicated groups and scalarizes groups with slicing,
   capture, or producer-side storage operations. A group with TTL activity decides
   each lane first. It then issues guarded `tick_block_batch()` and
   `skip_block_batch()` calls over the compacted ticking and dormant lanes. Generated background roots
   compact currently active authored nodes into forward, reverse, and Tock batch
   records. The initial eight-lane cap bounds adapter stack growth; Front 16 owns
   measured sizing and joint layout costing. Intrinsic replay eligibility now follows
   the normalized Tick block operation, including authored block and batch-only forms.
   Realtime lowering keeps a ready group scalar when its callback-facing writable
   storage overlaps another lane. It also keeps the group scalar when a lane uses the
   shared contiguous-input scratch area. Front 16 can plan these storage lifetimes and
   batch choices together after measurement.
   After the second verification run, `realtime_ready_group_batches()` in
   `lowering_plan.cpp` makes these decisions during planning. The planner returns
   rejected groups to the scalar schedule. It also returns the whole graph to the
   scalar schedule when ready-group ordering would exceed the stack budget, so
   batching never moves buffers to `NodeStorage`.
   Focused fixtures distinguish a real generated batch from scalar singleton fallback
   by recording the received lane count. They cover different configured values,
   separate lane port bindings, dependency-frontier grouping, aliased-storage scalar
   fallback, Tick, skip, forward and reverse propagation, Tock evaluation, and a
   batch-only Tick source executed through generated replay.

   This leaves Front 6 source-complete pending the collaborator's compilation and full
   suite run. Verification failure reopens the front at the failing boundary; a clean
   run closes it without another implementation pass.

   **Verification follow-up (2026-10-21; human re-run pending):** the first full run
   found six failures among 548 tests. Two required source corrections: shared event
   targets now receive a cold-computed earliest ordering origin so the realtime merge
   remains a direct metadata lookup, and retained Tick sample-output views no longer
   apply the callback latency offset a second time. Four failures were corrected test
   contracts (steady-storage materialization, required stable port identity, and two
   malformed persisted sample ranges). No new realtime allocation, graph walk, or
   exception branch was introduced. Front 6 stays open through re-verification.

   **Second verification follow-up (2026-10-21; human re-run pending):** 546 of 548
   tests passed. The remaining coverage fixture now supplies complete stable fan-in
   connection descriptions instead of propagation-only placeholders. The remaining
   runtime failure revealed that disconnected Tick output compact carry reserved its
   latency frames but did not classify them as future frames; `future_frames` now uses
   the output latency. This is metadata-only on the realtime path and preserves the
   existing single copy loop and shared Sequential pace cursor.

   **Completion verification (2026-10-21):** the collaborator's following full
   548-test run passed. Front 6 transition/state-continuity work is closed with no
   known correctness friction. Any future grouping of callback ordinal stores by
   node/pace must be justified by measurement and is ordinary runtime cleanup rather
   than incomplete transition behavior.

   **Front 2 and Front 4 completion verification (2026-10-23):** the collaborator's
   complete 592-test suite passed after the Front 4 background-delivery closure. This
   verifies Front 2's previously blocked replay and reverse-propagation fixtures,
   closes Front 2 and Front 4, and satisfies Front 7's remaining normalized
   block-replay dependency. The separate Front 6 follow-up for graph revisions that
   migrate a mixed `combines_invocation_supplied` input remains open.

   **Front 4 reopening (2026-10-23; not yet compiled or tested):** that follow-up was
   caused by Front 4 playback rebuilding background-fed Sequential history from the
   current snapshot. Slot-fed inputs with history now own a retained timeline that
   receives one block per callback, and a slot holds only the current window. The
   per-callback copy shrinks from `history + block` frames to `block` frames.
   Background transactions also stop materializing history ahead of playback.

   **Sequential cursor scope:** GraphJit continuous Tick storage already uses the
   proposed seek-independent scheme: one monotonic `uint64_t` cursor per distinct
   absolute pace, shared by every graph-owned continuous port at that pace. The
   `sequential_begin_ordinal` inside each reflected port binding is not independent
   persistent ring state; generated execution overwrites it with that callback's
   derived start ordinal before entering authored code. A later cleanup may group
   those per-port stores into one per-node/per-pace invocation value if measurement
   justifies the ABI change. Do not collapse all paces into one integer cursor, because
   their projected value counts differ, and do not force externally materialized Tick
   inputs into this domain: those private playback rings are intentionally rebuilt in
   visible `GlobalIndex` coordinates for each requested window.

   **Front 6 raw-migration audit correction (2026-10-24; human verification
   pending):** sample and event input-timeline raw regions no longer use
   generation-local bundle/port coordinates as cross-generation migration identity.
   They use the stable concrete input-port identity already recorded for Front 6.
   Inputs without that stable identity keep an empty raw identity and start from their
   initializer. Adversarial GraphJit fixtures now distinguish reused numeric
   coordinates belonging to different stable nodes from one stable port whose bundle
   handle changes. This is cold metadata only; it adds no realtime work or storage.
   The first full run passed 597 of 598 tests. Its sole failure was a defect in the new
   shifted-bundle fixture: `GraphBuilderNodeBundles::import_child()` clears inverse
   virtual-node handles, and the fixture had restored only the forward virtual-node
   records. The fixture now restores both directions while retaining the original
   virtual-node IDs. Production source was unchanged by that follow-up; verification
   is pending.

   The collaborator's next full run passed all 598 tests. This result verifies the
   stable input-port migration identities and the corrected bundle-handle-shift
   fixture. The bounded Front 6 correction is complete, and Front 3 is the next
   dependency-ready correctness front.

   Front 3 source is now a completion candidate. It carries logical continuous and
   event Region lists through propagation, transaction storage, persisted publication,
   and Tick materialization. `BackgroundStorageRealization` pre-allocates one independent
   buffer per selected Region and shares one producer-home allocation between the
   output's physical storage roles. Coverage-to-Sequential reduction and Tick
   playback use the target input's generated initializer and combiner. The
   collaborator's 2026-10-25 build and full run passed all 625 tests. Follow-up review
   added the missing postfix increment required by the declared forward iterator and
   a compiled authored-node multi-Region integration test. The event extension gives
   every event Region independent storage, preserves overlaps and duplicates, and
   prebuilds the merged event timeline used by Sequential Tick consumers. The public
   propagation operation is now `publish(...)` for both port kinds, with its argument
   selected statically from Region versus Coverage production. These corrections and
   the new event adversarial tests await the next collaborator run. Final static
   review also closed event output demands over complete logical Regions before
   reverse propagation, preserved current logical order when Tick materialization
   mixes recomputed and retained records, rejected event coverage
   migration across different production kinds, and required nonempty external event
   views to carry Region descriptors and per-Region callbacks. Contextual replayed
   Tick outputs continue to use positional publication internally; narrow replay
   demand is no longer expanded to the complete canonical output coverage, and
   persisted replay records use positional overwrite instead of authored logical
   Region-batch replacement. These final corrections also await the next collaborator
   run.

3. Complete realtime persisted playback and recording span operations. Persisted
   playback now clears only requested sample windows, traverses relevant sample
   regions, copies ring-wrap runs, and binary-searches clipped event chunks. Realtime
   recording still needs bulk ring capture; direct materialization indexing and cached
   materialization coverage offsets/frame counts also remain.
4. Make `BackgroundGeneration` own a sealed reusable runtime skeleton: reader slots,
   storage realization objects, binding adapters, node/operation frames, reusable replay
   buffers, and pre-indexed static routes. Transactions supply new root pins,
   coverage/selections, produced payload views, and logical reset state without
   reconstructing that topology. Stream queued block chains into bulk region construction
   and merge sorted event ranges rather than staging and sorting individual values.
5. Replace the capacity manager's periodic producer scan with low-watermark and returned-
   block edge notifications, and replace per-output work wakes with one wake for all
   chains finalized by a realtime callback.
6. Optimize compiler plan searches and other cold metadata construction only after the
   runtime scaling work above.

## Additional ownership constraints

- A stable persisted-output identity and a generation-local output identity have
  different retirement rules even when both resolve to compact store slots. Slots must
  not be recycled while any snapshot, generation, queued route, or transition can still
  name them.
- Immutable background topology and transaction-resettable state are separate. Reusing a
  runtime skeleton must not let failed or stale transaction state leak into the next
  transaction.
- Current realtime queue blocks return through generation-owned producer reserves. A
  persisted record cannot retain one of those blocks after that reserve dies. Zero-copy
  queue-block-to-record promotion therefore requires an explicit transfer to
  executor-level ownership (or equivalent record backing), not merely retaining the
  existing block.
- Retired-root reclamation must be serviced internally or guaranteed by the host, and
  opportunistic background reclamation must have a bounded budget so it cannot create an
  unbounded actor stall.

## Known costs covered by this roadmap

- linear persisted record and coverage lookup;
- linear record insertion, erasure, and mutation deduplication;
- full record-pointer copying, full metadata reconstruction, and history-sized reference
  release for every persisted snapshot successor;
- per-frame/per-channel persisted playback and per-value realtime capture;
- retained-event-ring rescans for every callback;
- linear Tick-materialization lookup and repeated coverage-prefix walks;
- per-value sample-region reconstruction and concatenate-plus-sort event-chunk rebuilding;
- synchronous unbounded graph-cutover copying;
- per-transaction reader registration and reconstruction of background realization and
  call-frame topology;
- per-publication persisted-state allocation and reader-list mutex traffic;
- repeated runtime discovery of static port/storage/route relationships;
- one-millisecond capacity polling, per-output background notification, and reclamation
  that currently depends on an explicit servicing command;
- lower-priority stack-pressure replanning and compiler metadata searches.

This is a static-review priority order, not benchmark evidence. Measurements should
validate constants and later optimization choices, but no measurement is needed to
justify removing work whose asymptotic cost grows with the complete recording on the
realtime or successor-publication path.
