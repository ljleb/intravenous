# Original findings for scope edits, control bindings, and Event planning

These rows appeared in the runtime responsibility audit on 2026-11-21.
The [current audit](../runtime_responsibility_audit.md#scope-edits-control-bindings-and-event-planning)
records the implementation changes and the remaining Event planning work.

| Current operation | Work that can be avoided | Where the decision belongs |
| --- | --- | --- |
| [`Observations::handle_protocol`](../../src/intravenous/observations/observations.cpp#L128) modifies `scopes_` before counting all windows and calling `reconcile_locked()`. On failure it uses `accepted_.revision` to choose whether to restore the old `Scope`. | A request that exceeds a limit copies its old interests, modifies `scopes_`, and then puts the old scope back. `reconcile_locked()` can also consume probe IDs before discovering a limit error. | Derive the combined interests from the existing scopes plus one proposed scope change. Validate that result and allocate IDs locally. Commit the proposed scopes and requirements after local validation and request acceptance. Keep desired requirements when the existing protocol calls for retry after compilation failure. |
| [`ProjectGraph::rebuild_locked`](../../src/intravenous/project/project_graph.cpp#L1558) appends `ConfiguredManualControl` bindings one by one. A virtual selector that cannot bind every selected bundle then truncates the output vector. [`handle_project_upsert_manual_control`](../../src/intravenous/project/project_graph.cpp#L3071) also changes `manual_controls_` first and restores the copied vector if rebuilding throws. | The first case inserts bindings that cannot participate. The second makes rebuild depend on provisional edits to stored project controls. | A binding operation can return the complete set of bindings and diagnostics for a `ManualControlId`. Pass proposed controls into configuration and give the project edit path the result it needs to distinguish rejected edits from accepted edits awaiting execution. The same binding result can inform graph scope metadata. |
| [`plan_event_ports_once`](../../src/intravenous/graph_jit/lowering_plan.cpp#L4624) builds an `EventPortBindingPlan` to tally operations, selects `EventPlanningDecisions`, and calls itself with the choices. | It rebuilds the event operation topology to apply storage choices that the first pass could describe separately. | Return event delivery operations and their storage requirements from the first pass. Choose realized storage from those requirements. Bind callback slots and layout once after the choice. The operation inventory is also a useful input to event state transfer and execution tests. |

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
