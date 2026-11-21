# Runtime responsibility audit

This is an implementation audit from 2026-11-21. The files below are the
project-owned source and test files above 4,000 lines in this checkout. The
review also followed related work in `Observations`. It excludes vendored
miniaudio. The suggested boundaries are candidates for future changes.

## Decisions that come after the work

| Current operation | Work that can be avoided | Where the decision belongs |
| --- | --- | --- |
| [`Observations::handle_protocol`](../src/intravenous/runtime/observations.cpp#L104) modifies `scopes_` before counting all windows and calling `reconcile_locked()`. On failure it uses `accepted_.revision` to choose whether to restore the old `Scope`. | A request that exceeds a limit copies its old interests, modifies `scopes_`, and then puts the old scope back. `reconcile_locked()` can also consume probe IDs before discovering a limit error. | Derive the combined interests from the existing scopes plus one proposed scope change. Validate that result and allocate IDs locally. Commit the proposed scopes and requirements after local validation and request acceptance. Keep desired requirements when the existing protocol calls for retry after compilation failure. |
| [`ProjectGraph::rebuild_locked`](../src/intravenous/runtime/project_graph.cpp#L1696) appends `ConfiguredManualControl` bindings one by one. A virtual selector that cannot bind every selected bundle then truncates the output vector. [`handle_project_upsert_manual_control`](../src/intravenous/runtime/project_graph.cpp#L3045) also changes `manual_controls_` first and restores the copied vector if rebuilding throws. | The first case inserts bindings that cannot participate. The second makes rebuild depend on provisional edits to stored project controls. | A binding operation can return the complete set of bindings and diagnostics for a `ManualControlId`. Pass proposed controls into configuration and give the project edit path the result it needs to distinguish rejected edits from accepted edits awaiting execution. The same binding result can inform graph scope metadata. |
| [`ConnectionTopology::build`](../src/intravenous/graph_jit/connection_plan.cpp#L6886) copies the `ConfiguredGraph` and clears `override_connections` for authored validation. It then builds the graph with the override in effect. | The authored validation calls `build_connection_analysis_plan()` and discards a result that includes scheduling and physical storage decisions. | Preserve the normalized authored routes and the temporal cuts that justify `detach()`. Apply the override to those routes and run physical planning on the effective graph. Keep authored validation independent so a live override cannot conceal an invalid authored graph. |
| [`ProjectGraph::compile_graph`](../src/intravenous/runtime/project_graph.cpp#L712) requests compilation with a predecessor when bindings alone cannot satisfy a probe change. [`GraphJit`](../src/intravenous/runtime/graph_jit.cpp#L2234) derives transition inputs before the executor checks if the compiled code fits the active layout. | Compatible observation code can leave a state-transfer plan unused. The attempt-ID change avoids reserving an execution generation in this case. | Check compatibility against the steady lowering result first, then build the transition only when the check calls for a cutover. Reuse the steady result in either path. |
| [`plan_event_ports_once`](../src/intravenous/graph_jit/lowering_plan.cpp#L4624) builds an `EventPortBindingPlan` to tally operations, selects `EventPlanningDecisions`, and calls itself with the choices. | It rebuilds the event operation topology to apply storage choices that the first pass could describe separately. | Return event delivery operations and their storage requirements from the first pass. Choose realized storage from those requirements. Bind callback slots and layout once after the choice. The operation inventory is also a useful input to event state transfer and execution tests. |
| [`build_lowering_plan`](../src/intravenous/graph_jit/lowering_plan.cpp#L9944) retries `plan_realtime_ports()` when packed stack size exceeds the budget. After building declarations and imports it can reject a batch and restart planning. | Each stack trial repeats parts of connection analysis and configures background replay. A rejected batch discards declarations and imports as well. | Give storage selection stable inputs for a given callback order and reserve replanning for facts that change with that order. Check batch compatibility as soon as the necessary layouts and callback signatures are known. The packed high-water mark may still require a search. |

A failed executor cutover is different. Preparation can become stale against
the active predecessor after a candidate was eligible. The executor needs to
discard that preparation and rebase accepted successors. Likewise, a private
Tock preview can discard derived results while committed Buffered Sequential
recordings stay with `PersistedDataStore`.

## Ownership in the large source files

| File | Lines | Boundary to scrutinize |
| --- | ---: | --- |
| [`lowering_plan.cpp`](../src/intravenous/graph_jit/lowering_plan.cpp#L9656) | 10,328 | `build_lowering_plan()` coordinates connection analysis, storage choice, package imports and scheduling. Event delivery, storage requirements and callback binding have different inputs and can be tested separately. Start with `plan_event_ports_once()` because its second pass reveals where the current record loses those intermediate facts. |
| [`connection_plan.cpp`](../src/intravenous/graph_jit/connection_plan.cpp#L7021) | 7,245 | `ConnectionTopology::build_analysis_plan()` turns routes into a schedule, derives compensation and in-flight requirements, and selects storage and background bindings. Keep authored validity and route normalization independent of scheduling and allocation. An override or storage policy change should reuse the decisions it cannot affect. |
| [`realtime_root.cpp`](../src/intravenous/graph_jit/realtime_root.cpp#L2871) | 5,757 | `RootPlanBuilder::build()` validates addresses and materializes primitive bindings, ready groups and region specializations. `execute_realtime_root()` consumes a completed `RealtimeRootPlan` on the audio thread. Use that plan as the boundary between cold binding and realtime execution, including the layout checks that the executor can then rely on. |
| [`project_graph.cpp`](../src/intravenous/runtime/project_graph.cpp#L1379) | 4,383 | `rebuild_locked()` handles authored edits, calls instance and connection configuration, binds controls, and publishes a snapshot. Control binding is a reusable operation on a finished `ConfiguredGraph` and its placements. Read-model projection, edit history and executor preparation can consume the result. `compile_graph()` now gives preparation and execution independent identities, with configuration and read-model construction still in this file. |
| [`persisted_data_store.cpp`](../src/intravenous/runtime/persisted_data_store.cpp#L3074) | 4,046 | `prepare_cache()` combines victim ranking, sample and event byte estimates, and reader-lease release. `Candidate::overwrite()` and the Region replacement functions encode snapshot and recording semantics. Expose eligible, reclaimable payloads and their byte costs to eviction policy. Keep authorization to overwrite recordings and the lifetime of pinned payloads in the store. |

The [shared GraphJit runtime package fixture](../tests/modules/graph_jit/package_fixture.cpp#L8020)
has 8,095 lines. Its raw-string chunks have a compiler size limit, so adding
a fixture for one behavior can break a chunk containing unrelated fixtures.
Actual package source files grouped by behavior can still share one built
artifact and the current workspace lock. The [connection-planning tests](../tests/graph_jit_connection_plan_gtest.cpp)
(4,988 lines), [project graph tests](../tests/modules/project_graph_gtest.cpp)
(4,996 lines), and [executor tests](../tests/graph_executor_gtest.cpp)
(4,841 lines) can follow production boundaries as they become explicit.
Useful divisions include authored routing versus scheduling, project edits
versus execution publication, and cutover versus background Coverage.

`Observations` scope validation is the next small change with a clear owner.
For GraphJit, first measure the time in the two Event planning passes and in
the stack-budget retries. Those measurements can show how much work a
reusable operation inventory can remove from the compilation path.
