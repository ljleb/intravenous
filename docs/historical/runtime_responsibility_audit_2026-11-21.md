# Original findings for scope edits, control bindings, and Event planning

These rows appeared in the runtime responsibility audit on 2026-11-21.
The [current audit](../runtime_responsibility_audit.md#scope-edits-control-bindings-and-event-planning)
records the implementation changes.

| Current operation | Work that can be avoided | Where the decision belongs |
| --- | --- | --- |
| [`Observations::handle_protocol`](../../src/intravenous/observations/observations.cpp#L128) modifies `scopes_` before counting all windows and calling `reconcile_locked()`. On failure it uses `accepted_.revision` to choose whether to restore the old `Scope`. | A request that exceeds a limit copies its old interests, modifies `scopes_`, and then puts the old scope back. `reconcile_locked()` can also consume probe IDs before discovering a limit error. | Derive the combined interests from the existing scopes plus one proposed scope change. Validate that result and allocate IDs locally. Commit the proposed scopes and requirements after local validation and request acceptance. Keep desired requirements when the existing protocol calls for retry after compilation failure. |
| [`ProjectGraph::rebuild_locked`](../../src/intravenous/project/project_graph.cpp#L1558) appends `ConfiguredManualControl` bindings one by one. A virtual selector that cannot bind every selected bundle then truncates the output vector. [`handle_project_upsert_manual_control`](../../src/intravenous/project/project_graph.cpp#L3071) also changes `manual_controls_` first and restores the copied vector if rebuilding throws. | The first case inserts bindings that cannot participate. The second makes rebuild depend on provisional edits to stored project controls. | A binding operation can return the complete set of bindings and diagnostics for a `ManualControlId`. Pass proposed controls into configuration and give the project edit path the result it needs to distinguish rejected edits from accepted edits awaiting execution. The same binding result can inform graph scope metadata. |
| [`plan_event_ports_once`](../../src/intravenous/graph_jit/lowering_plan.cpp#L4624) builds an `EventPortBindingPlan` to tally operations, selects `EventPlanningDecisions`, and calls itself with the choices. | It rebuilds the event operation topology to apply storage choices that the first pass could describe separately. | Return event delivery operations and their storage requirements from the first pass. Choose realized storage from those requirements. Bind callback slots and layout once after the choice. The operation inventory is also a useful input to event state transfer and execution tests. |

## Event binding before storage and routing were separate

This finding described the implementation after the first split of Event
costing and byte layout. At that point, `plan_event_ports()` reused its
first bindings when the selected storage kinds matched them. See the
[completed Event binding work](../runtime_responsibility_audit.md#event-binding).

| Current operation | Work that can be avoided | Where the decision belongs |
| --- | --- | --- |
| [`plan_event_ports()`](../../src/intravenous/graph_jit/lowering_plan.cpp#L8592) binds the connected streams again if exact operation costs change a storage kind. | The second binding uses the same node inventory. It still rebuilds the connected operations and their callback slots if the first binding used a different raw output alias, carry buffer, or sequence capacity. | Give each operation a logical stream identity and record how its capacity depends on the source streams. Resolve aliases, capacity, and storage after choosing the storage kinds. Keep one record for shared conversions so the new plan counts each conversion once. |

## Directory proposal before the move

This proposal preceded the split of `src/intravenous/runtime/`. The
[current layout](../runtime_responsibility_audit.md#directory-layout-and-dependency-boundaries)
records where the files went.


`src/intravenous/runtime/` contains 193 files, including 60 bridge headers
and sources. It mixes project editing, audio execution, and JSON-RPC in one
directory. The CMake targets explain part of the split. The GraphJit object
target includes execution and storage. The tooling object target includes
project editing, packages, device I/O, and the server transport.
[Runtime targets](../../src/intravenous/CMakeLists.txt#L340)

| Proposed location | Responsibility and source examples |
| --- | --- |
| `runtime/project/` | Authored instances, connections, selectors, and edit history. Move `project_graph`, `graph_connections`, `node_instances`, and `project_node_path_resolver` here. Put `project_persistence`, `project_autosave`, and the project file codecs in `project/persistence/`. |
| `runtime/execution/` | Activation, playback, and Tick invocation. Move `graph_executor` and the Tick invocation files here. Give Coverage evaluation and storage their own `execution/background/` and `execution/storage/` directories. |
| `graph_jit/` | Compilation of a configured graph. Move `graph_jit/graph_jit.cpp` and its interface next to the lowering and materialization code that they call. Keep project-specific request adapters with the application wiring. |
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


## Definition catalog, shared test fixture, and cache retention

These findings preceded the changes recorded in the [current audit](../runtime_responsibility_audit.md#catalog-publication-test-fixture-and-cache-retention).

| Current operation | Work that can be avoided | Where the decision belongs |
| --- | --- | --- |
| [`ProjectGraph::handle_node_definitions_snapshot_changed()`](../../src/intravenous/project/project_graph.cpp#L2376) replaces `definitions_snapshot_` before it checks whether configuration needs to change. If rebuilding throws, it restores the old snapshot. | Configuration reads the proposed snapshot through the same field that stores the accepted catalog. The caller needs a restore path to keep a failed publication retryable. | Pass the proposed `NodeDefinitionsSnapshot` to configuration. Commit the desired catalog when the publication succeeds. Preserve the configured generation's provider closure until the replacement graph is ready. |

The [shared GraphJit runtime package fixture](../../tests/modules/graph_jit/package_fixture.cpp#L8020)
has 8,095 lines. Its raw-string chunks have a compiler size limit, so adding
a fixture for one behavior can break a chunk containing unrelated fixtures.
Actual package source files grouped by behavior can still share one built
artifact and the current workspace lock.

In `PersistedDataStore::prepare_cache()`, victim ranking, payload byte estimates, and lease release shared the same implementation. The store also held the rules for overwriting recordings and retaining pinned snapshots.

## Manual override validation before the planning split

This row appeared in the runtime responsibility audit on 2026-11-21. The [current audit](../runtime_responsibility_audit.md#manual-overrides-and-authored-connections) records how authored connection validation uses shared routing facts.

| Current operation | Work that can be avoided | Where the decision belongs |
| --- | --- | --- |
| [`ConnectionTopology::build`](../../src/intravenous/graph_jit/connection_plan.cpp#L6886) copies the `ConfiguredGraph` and clears `override_connections` for authored validation. It then builds the graph with the override in effect. | The authored validation calls `build_connection_analysis_plan()` and discards a result that includes scheduling and physical storage decisions. | Preserve the normalized authored routes and the temporal cuts that justify `detach()`. Apply the override to those routes and run physical planning on the effective graph. Keep authored validation independent so a live override cannot conceal an invalid authored graph. |

## Observation compilation before checking the steady layout

This row appeared in the runtime responsibility audit on 2026-11-21. The [current audit](../runtime_responsibility_audit.md#observation-compilation) records where the compiler now checks the active layout.

| Current operation | Work that can be avoided | Where the decision belongs |
| --- | --- | --- |
| [`ProjectGraph::compile_graph`](../../src/intravenous/project/project_graph.cpp#L712) requests compilation with a predecessor when bindings alone cannot satisfy a probe change. [`GraphJit`](../../src/intravenous/graph_jit/graph_jit.cpp#L2234) derives transition inputs before the executor checks if the compiled code fits the active layout. | Compatible observation code can leave a state-transfer plan unused. The attempt-ID change avoids reserving an execution generation in this case. | Check compatibility against the steady lowering result first, then build the transition only when the check calls for a cutover. Reuse the steady result in either path. |

## Stack-budget retries before schedule reuse

The runtime responsibility audit recorded the repeated planning below on
2026-11-21. See the [current implementation](../runtime_responsibility_audit.md#stack-budget-retries).

| Current operation | Work that can be avoided | Where the decision belongs |
| --- | --- | --- |
| [`build_lowering_plan`](../../src/intravenous/graph_jit/lowering_plan.cpp#L9732) retries `plan_realtime_ports()` when packed stack size exceeds the budget. After building declarations and imports it can reject a batch and restart planning. | Each stack trial repeats parts of connection analysis and configures background replay. A rejected batch discards declarations and imports as well. | Give storage selection stable inputs for a given callback order and reserve replanning for facts that change with that order. Check batch compatibility as soon as the necessary layouts and callback signatures are known. The packed high-water mark may still require a search. |

The [storage planning status](../sequential_port_storage_planning.md#current-implementation-status)
described the path from connection analysis to packed stack allocation as follows.

- the fixed 64-event and 16-KiB thresholds are gone. The shared pure chooser
  now enumerates transient, carry, and full-persistent candidates, exposes their
  copied bytes, ring-addressed bytes, stack footprint, persistent footprint, and
  weighted cost, and accepts candidate-specific whole-group copy counts plus a
  configured stack-budget constraint. `GraphJit` now propagates one configured
  cost model through connection, sample, event, and feedback planning. After the
  sample/event stack buffers are independently lifetime-packed, lowering lays
  those two byte ranges into one generated-root stack allocation and enforces
  the budget against that final byte count. If it does not fit, lowering raises
  the stack-byte cost and re-runs storage selection so eligible producer,
  history/latency, feedback, and disconnected-output buffers move to
  `NodeStorage`; a final minimum-stack pass either fits or reports that the
  remaining mandatory conversion/merge buffers exceed the budget. The default
  weights preserve the landed block-relative/rate-derived crossover until
  topology-specific costs justify an earlier full-persistent choice;

## Event port planner extraction proposal

The [runtime responsibility audit](../runtime_responsibility_audit.md#event-port-planning)
recorded this proposal on 2026-11-21 before the source moved. The line
reference describes the file at that time.

### Event port planning

About 3,800 lines in `lowering_plan.cpp` produce one
`EventPortBindingPlan`. The planner has a single caller,
`build_lowering_plan()`. The proposed first split moves
`bind_event_ports()`, `realize_event_storage()`, `layout_event_ports()`,
and their plan records to `graph_jit/lowering/event_port_plan.{h,cpp}`.
This split keeps the Event planning behavior intact and leaves
`build_lowering_plan()` in charge of the stack budget for Sample and Event
storage.
[Event port planner](../../src/intravenous/graph_jit/lowering_plan.cpp#L4610)

The large-file inventory described the same boundary as follows.

| File | Lines | Boundary to scrutinize |
| --- | ---: | --- |
| [`lowering_plan.cpp`](../../src/intravenous/graph_jit/lowering_plan.cpp#L9458) | 10,061 | `build_lowering_plan()` coordinates connection analysis, storage choice, package imports and scheduling. Event delivery, storage requirements and callback binding have different inputs and can be tested separately. The split between `select_event_storage()` and `layout_event_ports()` makes those responsibilities explicit. Storage trials share one connection analysis per callback order. |
