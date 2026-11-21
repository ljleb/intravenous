# Front 12 Implementation Handoff Record

_Historical status: non-normative. This file preserves the implementation handoff
record of Front 12 (scoped `BuilderSession` / `SubgraphClosure`) as it stood when the
front closed. The record asked to be removed or moved to `docs/historical/` at that
point._

_Source: the section "Front 12: scoped `BuilderSession` / `SubgraphClosure`" of
[Implementation Fronts Direction](../implementation_fronts_direction.md), preserved
2026-11-06 when the repository owner closed Front 12 after the complete test suite
passed with the final change (throw-site stack capture) included._

_Reading note: the record grew in order. Its "Implemented representation" list near
the end was written before decision F12-14 and still describes the journaled
rollback, checkpoints, and transaction stack that F12-14 removed. The F12-14 entry
describes what replaced them._

_Current replacement: the closure status of that Front 12 section, which summarizes
the delivered representation and lists the deferred F12-7 extension._

## Preserved record

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
