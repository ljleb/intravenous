# Runtime responsibility audit

This is an implementation audit from 2026-11-21. The files below are the
project-owned source and test files above 4,000 lines in this checkout. The
review also followed related work in `Observations`. It excludes vendored
miniaudio. The suggested boundaries are candidates for future changes.

## Scope edits, control bindings, and Event planning

- `Observations::handle_protocol()` checks aggregate limits against the proposed
  scope. `reconcile_locked()` allocates probe IDs locally. A rejected scope
  leaves the subscription and ID sequence intact. The service still retains
  accepted requirements when compilation reports an error.
- `configure_manual_controls()` resolves the ports in a virtual selection
  before adding bindings to `ConfiguredGraph`. `ProjectGraph::rebuild_locked()`
  takes proposed manual controls separately from the stored controls.
  Accepted edits keep their authored controls when configuration fails.
- `select_event_storage()` reads the costs of the shared conversions and
  feedback streams. `layout_event_ports()` computes byte offsets and assigns
  stack space once. When the selected kinds match the initial bindings,
  `plan_event_ports()` reuses those bindings and updates their costs.

The [original findings](./historical/runtime_responsibility_audit_2026-11-21.md)
describe the earlier behavior.

## Decisions that come after the work

| Current operation | Work that can be avoided | Where the decision belongs |
| --- | --- | --- |
| [`ConnectionTopology::build`](../src/intravenous/graph_jit/connection_plan.cpp#L6886) copies the `ConfiguredGraph` and clears `override_connections` for authored validation. It then builds the graph with the override in effect. | The authored validation calls `build_connection_analysis_plan()` and discards a result that includes scheduling and physical storage decisions. | Preserve the normalized authored routes and the temporal cuts that justify `detach()`. Apply the override to those routes and run physical planning on the effective graph. Keep authored validation independent so a live override cannot conceal an invalid authored graph. |
| [`ProjectGraph::compile_graph`](../src/intravenous/runtime/project_graph.cpp#L712) requests compilation with a predecessor when bindings alone cannot satisfy a probe change. [`GraphJit`](../src/intravenous/runtime/graph_jit.cpp#L2234) derives transition inputs before the executor checks if the compiled code fits the active layout. | Compatible observation code can leave a state-transfer plan unused. The attempt-ID change avoids reserving an execution generation in this case. | Check compatibility against the steady lowering result first, then build the transition only when the check calls for a cutover. Reuse the steady result in either path. |
| [`plan_event_ports()`](../src/intravenous/graph_jit/lowering_plan.cpp#L8592) binds the connected streams again if exact operation costs change a storage kind. | The second binding uses the same node inventory. It still rebuilds the connected operations and their callback slots if the first binding used a different raw output alias, carry buffer, or sequence capacity. | Give each operation a logical stream identity and record how its capacity depends on the source streams. Resolve aliases, capacity, and storage after choosing the storage kinds. Keep one record for shared conversions so the new plan counts each conversion once. |
| [`build_lowering_plan`](../src/intravenous/graph_jit/lowering_plan.cpp#L9903) retries `plan_realtime_ports()` when packed stack size exceeds the budget. After building declarations and imports it can reject a batch and restart planning. | Each stack trial repeats parts of connection analysis and configures background replay. A rejected batch discards declarations and imports as well. | Give storage selection stable inputs for a given callback order and reserve replanning for facts that change with that order. Check batch compatibility as soon as the necessary layouts and callback signatures are known. The packed high-water mark may still require a search. |
| [`ProjectGraph::handle_node_definitions_snapshot_changed()`](../src/intravenous/runtime/project_graph.cpp#L2376) replaces `definitions_snapshot_` before it checks whether configuration needs to change. If rebuilding throws, it restores the old snapshot. | Configuration reads the proposed snapshot through the same field that stores the accepted catalog. The caller needs a restore path to keep a failed publication retryable. | Pass the proposed `NodeDefinitionsSnapshot` to configuration. Commit the desired catalog when the publication succeeds. Preserve the configured generation's provider closure until the replacement graph is ready. |

A failed executor cutover is different. Preparation can become stale against
the active predecessor after a candidate was eligible. The executor needs to
discard that preparation and rebase accepted successors. Likewise, a private
Tock preview can discard derived results while committed Buffered Sequential
recordings stay with `PersistedDataStore`.

## Ownership in the large source files

| File | Lines | Boundary to scrutinize |
| --- | ---: | --- |
| [`lowering_plan.cpp`](../src/intravenous/graph_jit/lowering_plan.cpp#L9615) | 10,287 | `build_lowering_plan()` coordinates connection analysis, storage choice, package imports and scheduling. Event delivery, storage requirements and callback binding have different inputs and can be tested separately. The split between `select_event_storage()` and `layout_event_ports()` makes those responsibilities explicit. The connected-stream bindings still depend on the chosen storage kind. |
| [`connection_plan.cpp`](../src/intravenous/graph_jit/connection_plan.cpp#L7021) | 7,245 | `ConnectionTopology::build_analysis_plan()` turns routes into a schedule, derives compensation and in-flight requirements, and selects storage and background bindings. Keep authored validity and route normalization independent of scheduling and allocation. An override or storage policy change should reuse the decisions it cannot affect. |
| [`realtime_root.cpp`](../src/intravenous/graph_jit/realtime_root.cpp#L2871) | 5,757 | `RootPlanBuilder::build()` validates addresses and materializes primitive bindings, ready groups and region specializations. `execute_realtime_root()` consumes a completed `RealtimeRootPlan` on the audio thread. Use that plan as the boundary between cold binding and realtime execution, including the layout checks that the executor can then rely on. |
| [`project_graph.cpp`](../src/intravenous/runtime/project_graph.cpp#L1558) | 4,390 | `rebuild_locked()` handles authored edits, calls instance and connection configuration, and publishes a snapshot. `configure_manual_controls()` now owns control binding on a finished `ConfiguredGraph`. Read-model projection, edit history, and executor preparation can consume that result. |
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

## Directory layout and dependency boundaries

`src/intravenous/runtime/` contains 193 files, including 60 bridge headers
and sources. It mixes project editing, audio execution, and JSON-RPC in one
directory. The CMake targets explain part of the split. The GraphJit object
target includes execution and storage. The tooling object target includes
project editing, packages, device I/O, and the server transport.
[Runtime targets](../src/intravenous/CMakeLists.txt#L340)

| Proposed location | Responsibility and source examples |
| --- | --- |
| `runtime/project/` | Authored instances, connections, selectors, and edit history. Move `project_graph`, `graph_connections`, `node_instances`, and `project_node_path_resolver` here. Put `project_persistence`, `project_autosave`, and the project file codecs in `project/persistence/`. |
| `runtime/execution/` | Activation, playback, and Tick invocation. Move `graph_executor` and the Tick invocation files here. Give Coverage evaluation and storage their own `execution/background/` and `execution/storage/` directories. |
| `graph_jit/` | Compilation of a configured graph. Move `runtime/graph_jit.cpp` and its interface next to the lowering and materialization code that they call. Keep project-specific request adapters with the application wiring. |
| `runtime/packages/` | Package discovery and build, definition registration, and source introspection. Keep the package build records here. Expose the node definition snapshot to graph configuration through its own header. |
| `runtime/observations/` | Scope interests, window queries, scheduling, and delivery. Give `Observations` a disconnect record with client identity and move JSON-RPC parsing to the transport. Today `observations.h` includes `socket_rpc_requests.h` for disconnect. |
| `runtime/rpc/` and `runtime/audio/` | Keep parsing, reply serialization, and client connections in RPC. Keep device callbacks and input synchronization in audio. Both use runtime contracts to request work and report status. |

Keep `app.cpp`, startup, and cross-module bridges at the application boundary.
A dedicated `runtime/wiring/` directory can hold the bridge files that join
two owners. The files that declare an owner's request records and events stay
with that owner. This keeps the linker event test bindings usable.

Start with a directory move that preserves the current target and event
boundaries. Split shared headers as clients move. `Observations` can depend
on its own requests without pulling in the RPC parser. `GraphJit` can expose
compiled artifacts without depending on `ProjectGraph`'s transaction record.
Those changes reduce the headers a new caller needs to read and include.
