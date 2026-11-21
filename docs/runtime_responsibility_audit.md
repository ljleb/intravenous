# Runtime responsibility audit

This implementation audit began on 2026-11-21. Its inventory covers
project-owned runtime source and test files above 4,000 lines. It excludes
vendored miniaudio. The findings also cover related work across file
boundaries. The project owner reported that all tests passed after the
realtime root extraction.

## Completed work

### Scope edits, control bindings, and Event planning

- `Observations::handle_protocol()` checks aggregate limits against the proposed
  scope. `reconcile_locked()` allocates probe IDs locally. A rejected scope
  leaves the subscription and ID sequence intact. The service still retains
  accepted requirements when compilation reports an error.
- `configure_manual_controls()` resolves the ports in a virtual selection
  before adding bindings to `ConfiguredGraph`. `ProjectGraph::rebuild_locked()`
  takes proposed manual controls separately from the stored controls.
  Accepted edits keep their authored controls when configuration fails.

The [Event binding](#event-binding) section records the separate work on
Event planning.
The [original findings](./historical/runtime_responsibility_audit_2026-11-21.md)
describe the earlier behavior.

### Catalog publication, test fixture, and cache retention

- `ProjectGraph::rebuild_locked()` receives a proposed definitions snapshot
  from `handle_node_definitions_snapshot_changed()`. The configured
  generation pins the definitions used for its build. A reported configuration
  failure still accepts the desired catalog. An exception leaves the
  publication eligible for retry.
- The [shared GraphJit runtime test package](../tests/test_modules/graph_jit_runtime_shared_package/module.cpp)
  uses source fragments grouped by behavior. CTest still uses one shared
  artifact. Fixture code no longer has to fit inside C++ string literals in
  the test runner.
- [`CacheRetentionPolicy`](../src/intravenous/execution/storage/cache_retention_policy.h)
  chooses when to reclaim and which payloads to reclaim first.
  `PersistedDataStore::prepare_cache()` enumerates the payloads, combines
  required coverage by output, and releases leases. It checks allocator
  usage again after each reclamation. Recording and snapshot ownership
  stay in `PersistedDataStore`.

### Manual overrides and authored connections

`ConnectionTopology::build()` indexes structural Sample routing once and
shares the Event connection inventory between both views. Each view resolves
Sample routes with its own override rules, because replacing a module input
can change the sources of its downstream routes. Authored validation checks
feedback, `detach()`, replay dependencies, Region inputs, and complete-buffer
inputs. The effective routes drive the background schedule, the realtime
schedule, and physical storage planning.
[Connection topology](../src/intravenous/graph_jit/connection_plan.cpp#L4941)

### Connection routing, scheduling, and port grouping

`ConnectionTopology::build()` owns effective routing and authored validation.
`plan_realtime_schedule()` assigns Tick order and feedback regions, then
aligns feed-forward read latencies. Region formation and final ordering
share one list of Tick dependencies.
[Realtime scheduling](../src/intravenous/graph_jit/connections/realtime_schedule_plan.cpp#L648)

`plan_port_groups()` uses the resulting schedule and read latencies. It
partitions ports by their uses and records live intervals and storage
requirements for Sample and Event producer groups.
`ConnectionTopology::build_analysis_plan()` calls both planners and
completes the background bindings.
[Port grouping](../src/intravenous/graph_jit/connections/port_group_plan.cpp#L1347) · [Connection analysis](../src/intravenous/graph_jit/connection_plan.cpp#L5039)

### Realtime root construction and execution

`RootPlanBuilder` resolves native callbacks, validates bindings, and computes
scratch offsets before publication. `execute_realtime_root()` reads the
finished `RealtimeRootPlan` to run a block on the audio thread. The plan
definition is private to these two files.
[Root construction](../src/intravenous/graph_jit/realtime/root_plan.cpp#L274) ·
[Plan records](../src/intravenous/graph_jit/realtime/root_plan.h#L340) ·
[Root execution](../src/intravenous/graph_jit/realtime_root.cpp#L2233)

`PrimitiveExecutionStep::has_port_operations()` gives lowering and root
construction the same list of Sample and Event operations that prevent an
acyclic batch. Root construction checks the resolved Tick and skip callback
addresses and uses the capture flag it computed when it built each step.
[Port operations](../src/intravenous/graph_jit/lowering_plan.h#L206) ·
[Native batch validation](../src/intravenous/graph_jit/realtime/root_plan.cpp#L2243)

### Observation compilation

`ProjectGraph::compile_graph()` first tries to attach observation bindings to
the active code. When it needs new code, GraphJit builds one steady lowering
plan and compares its layout with the active execution generation. A
compatible plan skips Sequential state-transfer planning. An incompatible
plan supplies the steady layout for the cutover specialization in the same
compilation. The executor checks compatibility again when publishing the
artifact. If it declines and the observation request is still current,
`ProjectGraph` compiles a structural cutover.

### Event binding

`bind_event_ports()` records streams for producer groups and feedback,
their conversions, and the sequences that each Tick callback uses. It
sizes each working sequence from the current Event count and the count
that must survive between calls.

`realize_event_storage()` applies the selected storage kinds. It assigns
carry buffers and rings and resolves aliases that let a group reuse its
raw output. `layout_event_ports()` computes byte offsets and packs the
transient buffers once.

### Event port planning

`graph_jit/lowering/event_port_plan.{h,cpp}` owns the Event plan records,
stream binding, storage selection, and buffer packing.
`build_lowering_plan()` passes the graph, block size, and analysis into
`plan_event_ports()`. It uses the returned `EventPortBindingPlan` to
enforce the stack budget with the Sample plan.
[Event port planner](../src/intravenous/graph_jit/lowering/event_port_plan.cpp#L3629)

### Event conversion sharing

Event lowering uses one rule for reusing a materialization when it binds
a stream and when it resolves a raw output alias. A materialization produces
a sequence that another operation or callback can read. Shared requests
must agree on the source stream, conversion, scope, source filter, source
indices, and whether to select the invocation window.

Callback-facing conversions can cover the largest requested history. Each
callback reads its own window. A pre-merge conversion uses the merge's
exact history because the window determines the origin for Event ordering.
GraphJit widens the cost bounds of a shared conversion to cover every
request.
[Event materialization planner](../src/intravenous/graph_jit/lowering/event_port_plan.cpp#L337)

### Sample port planning

`graph_jit/lowering/sample_port_plan.{h,cpp}` owns the Tick Sample binding
records and `plan_sample_ports()`. `prepare_sample_storage_connections()`
selects the realtime part of mixed Tick and Tock routes and fills missing
`detach()` initial values once per callback order.

`SampleStorageConnections` supplies those routes by their original indices.
`build_sample_storage_plan()` reads the producer groups, schedule, and node
identities from the original analysis. Trials with the same callback order
share the prepared routes. Each trial chooses and packs storage for its
cost model.
[Sample route preparation](../src/intravenous/graph_jit/lowering/sample_port_plan.cpp#L225) ·
[Sample port planner](../src/intravenous/graph_jit/lowering/sample_port_plan.cpp#L284) ·
[Sample storage planner](../src/intravenous/graph_jit/sample_storage_plan.cpp#L791)

### Stack-budget retries

`ConnectionTopology::build_analysis_plan()` prepares one
`ConnectionAnalysisPlan` per callback order. Sample and Event storage
planners share its schedule, delivery routes, and buffer requirements
across trials. Each trial still prepares Sample and Event bindings, applies
the cost model, and packs the buffers to measure the stack usage.

GraphJit plans package imports from `ConnectionTopology` before the
trials. It fills in replay metadata, Event capture capacities, and
port-state transfer records after it accepts the schedule and layout.
[Stack-budget trials](../src/intravenous/graph_jit/lowering_plan.cpp#L3950)

### Executor cutover and Tock preview

An eligible candidate's cutover preparation can become stale against the
active predecessor. The executor then discards that preparation and rebases
accepted successors. A private Tock preview can discard derived results.
Committed Buffered Sequential recordings stay with `PersistedDataStore`.

### Directory layout and dependency boundaries

Application code now groups by component directly under
`src/intravenous/`. The GraphJit and tooling object targets still define
which object files each executable and test links. [CMake targets](../src/intravenous/CMakeLists.txt#L340)

| Directory | Responsibility |
| --- | --- |
| `project/` | Authored instances, connections, selectors, editing, and history. `project/persistence/` holds autosave and project file codecs. |
| `execution/` | Activation, playback, and Tick invocation. `execution/background/` owns Coverage evaluation. `execution/storage/` holds recording storage and the queues that feed it. |
| `graph_jit/` | Graph compilation, including the `GraphJit` entry point, lowering, and code materialization. |
| `packages/` | Package discovery and build, node definition registration, and source introspection. |
| `observations/` | Scope interests, window queries, scheduling, and result delivery. |
| `rpc/` | Socket transport, request parsing, and reply serialization. |
| `audio/` | System audio devices and input synchronization. |
| `app/` | Startup and composition. `app/bridges/` connects the components through linker events. Request and event declarations live with their owners. |

## Open findings and proposals

### Realtime batch selection

`realtime_ready_group_batches()` checks callback symbols, Tick capture counts,
and overlap between lanes when at least one access writes. Cyclic batches can
run Sample and Event operations around each phase. Root construction checks
cyclic phase structure again when it validates the native callbacks.

When lowering rejects a candidate batch, it removes its members from the
realtime batch classes and builds the schedule, declarations, and port plans
again. The packed layout is required to check overlap. Check which rules can
reject candidates earlier and which planned records can be reused after a
rejection.
[Batch eligibility](../src/intravenous/graph_jit/lowering_plan.cpp#L3232) ·
[Schedule retry](../src/intravenous/graph_jit/lowering_plan.cpp#L4136)

The Front 16 probe of `simple_sine` counted 17 scalar sine specializations
that shared one code address. It saw zero calls to `run_step_batch()` in
the traced block. The audit item above concerns compile-time retries.
Front 16 tracks measuring a block callback and a grouped specialization
for the realtime path.
[Oscillator batching probe](implementation_fronts_direction.md#additional-front-16-work-on-simple_sine-oscillator-batching)

### Ownership in the large source files

| File | Lines | Boundary to scrutinize |
| --- | ---: | --- |
| [`connection_plan.cpp`](../src/intravenous/graph_jit/connection_plan.cpp#L5039) | 5,275 | `ConnectionTopology::build()` resolves effective routes and validates authored routes. `build_analysis_plan()` calls the scheduling and grouping planners, inventories in-flight requirements, and records background bindings. |
| [`lowering_plan.cpp`](../src/intravenous/graph_jit/lowering_plan.cpp#L3700) | 4,314 | `build_lowering_plan()` coordinates connection analysis, storage trials, declarations, package imports, and execution planning. Sample and Event port planning have their own files. The outer loop still rebuilds the schedule, port plans, and declarations after a batch rejection. |
| [`project_graph.cpp`](../src/intravenous/project/project_graph.cpp#L1592) | 4,417 | `rebuild_locked()` handles authored edits, calls instance and connection configuration, and publishes a snapshot. `configure_manual_controls()` owns control binding on a finished `ConfiguredGraph`. `handle_socket_rpc_get_graph_scope()` also builds the editor's read model, including manual controls and execution status. |
| [`persisted_data_store.cpp`](../src/intravenous/execution/storage/persisted_data_store.cpp#L3209) | 4,044 | `Candidate::overwrite()` writes positional recordings. `replace_sample_regions()` and `replace_event_regions()` replace authored Regions in the candidate snapshot. The cache policy consumes payload costs and usage facts. The store keeps recording writes, payload estimates, and lease release. |

Review where read-model projection, edit history, and executor preparation
can consume the configured graph and its control bindings directly.
[Graph scope projection](../src/intravenous/project/project_graph.cpp#L3918)

The [connection-planning tests](../tests/graph_jit_connection_plan_gtest.cpp)
(5,070 lines), [project graph tests](../tests/modules/project_graph_gtest.cpp)
(5,028 lines), and [executor tests](../tests/graph_executor_gtest.cpp)
(4,841 lines) can follow production boundaries as they become explicit.
Useful divisions include authored routing versus scheduling, project edits
versus execution publication, and cutover versus background Coverage.

### Cross-component header dependencies

`Observations::handle_disconnect()` takes
`SocketRpcClientDisconnectedRequest`, so `observations.h` includes
`socket_rpc_requests.h`. A client identity record shared at the event
boundary can remove that dependency.
[Observations interface](../src/intravenous/observations/observations.h#L94)

`GraphJit::handle_project_graph_transaction()` takes
`ProjectGraphGraphJitRequest` through the application bridge. Review which
fields describe compilation and which describe project publication before
moving the project-facing request to that bridge.
[GraphJit request](../src/intravenous/graph_jit/graph_jit.h#L248) ·
[Application bridge](../src/intravenous/app/bridges/project_graph_graph_jit_bridge.cpp#L8)
