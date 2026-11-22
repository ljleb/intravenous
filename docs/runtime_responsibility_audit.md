# Runtime responsibility audit

This implementation audit began on 2026-11-21. The project owner reported
that all tests passed after the realtime root and Sample port planning
extractions. The [size inventory](#size-inventory) records the largest
project-owned functions and source files in the current checkout.

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

### Sample storage planning

`build_sample_storage_plan()` chooses storage for Sample producer groups
and feedback, binds connections to their representations, and packs the
transient arena. Local structs and lambdas hold much of that logic. Review
where each operation can take its input records and return its results
without sharing the whole function's local state.
[Sample storage planner](../src/intravenous/graph_jit/sample_storage_plan.cpp#L791)

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

| File | Boundary to scrutinize |
| --- | --- |
| [`connection_plan.cpp`](../src/intravenous/graph_jit/connection_plan.cpp#L5039) | `ConnectionTopology::build()` resolves effective routes and validates authored routes. `build_analysis_plan()` calls the scheduling and grouping planners, inventories in-flight requirements, and records background bindings. |
| [`lowering_plan.cpp`](../src/intravenous/graph_jit/lowering_plan.cpp#L3700) | `build_lowering_plan()` coordinates connection analysis, storage trials, declarations, package imports, and execution planning. Sample and Event port planning have their own files. The outer loop still rebuilds the schedule, port plans, and declarations after a batch rejection. |
| [`project_graph.cpp`](../src/intravenous/project/project_graph.cpp#L1592) | `rebuild_locked()` handles authored edits, calls instance and connection configuration, and publishes a snapshot. `configure_manual_controls()` owns control binding on a finished `ConfiguredGraph`. `handle_socket_rpc_get_graph_scope()` also builds the editor's read model, including manual controls and execution status. |
| [`persisted_data_store.cpp`](../src/intravenous/execution/storage/persisted_data_store.cpp#L3209) | `Candidate::overwrite()` writes positional recordings. `replace_sample_regions()` and `replace_event_regions()` replace authored Regions in the candidate snapshot. The cache policy consumes payload costs and usage facts. The store keeps recording writes, payload estimates, and lease release. |

Review where read-model projection, edit history, and executor preparation
can consume the configured graph and its control bindings directly.
[Graph scope projection](../src/intravenous/project/project_graph.cpp#L3918)

The connection-planning, project graph, and executor tests can follow
production boundaries as they become explicit. Useful divisions include
authored routing versus scheduling, project edits versus execution
publication, and cutover versus background Coverage.
[Connection-planning tests](../tests/graph_jit_connection_plan_gtest.cpp) ·
[Project graph tests](../tests/modules/project_graph_gtest.cpp) ·
[Executor tests](../tests/graph_executor_gtest.cpp)

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

## Size inventory

Snapshot from 2026-11-21. The scope is project-owned source and test code,
including the client, packages, and build tools. Vendored code, generated
files, data files, and documentation are outside this inventory. Both lists
run from longest to shortest. Size flags code for review.

### Functions over 100 code lines

The list stops at 100 entries. Each count covers the definition from its
signature through its closing line and excludes blank and comment-only
lines. It includes named functions, methods, and Google Test bodies. Inline
callbacks count toward their enclosing definition. Source parsing covers
C++ and TypeScript, including Vue script blocks. The C++ index has parse
gaps, so this is a first pass.

| Code lines | Function | Source |
| ---: | --- | --- |
| 2,569 | `bind_event_ports` | [event_port_plan.cpp:540](../src/intravenous/graph_jit/lowering/event_port_plan.cpp#L540) |
| 2,530 | `build_sample_storage_plan` | [sample_storage_plan.cpp:791](../src/intravenous/graph_jit/sample_storage_plan.cpp#L791) |
| 1,437 | `plan_sample_ports` | [sample_port_plan.cpp:284](../src/intravenous/graph_jit/lowering/sample_port_plan.cpp#L284) |
| 1,136 | `specialize_transition_plan` | [lowering.cpp:1336](../src/intravenous/graph_jit/lowering.cpp#L1336) |
| 989 | `build_background_storage_plan` | [background_storage_plan.cpp:420](../src/intravenous/graph_jit/background_storage_plan.cpp#L420) |
| 960 | `BackgroundEvaluationCallFrame::seal` | [background_evaluation_call_frame.cpp:815](../src/intravenous/execution/background/background_evaluation_call_frame.cpp#L815) |
| 852 | `plan_background_connections` | [connection_plan.cpp:3067](../src/intravenous/graph_jit/connection_plan.cpp#L3067) |
| 850 | `plan_declarations` | [lowering_plan.cpp:364](../src/intravenous/graph_jit/lowering_plan.cpp#L364) |
| 824 | `BackgroundStorageRealization::execute_operation` | [background_storage_realization.cpp:1744](../src/intravenous/execution/background/background_storage_realization.cpp#L1744) |
| 718 | `main` | [node_layout_storage.cpp:388](../tests/node_layout_storage.cpp#L388) |
| 683 | `define_background_root_operation` | [lowering.cpp:361](../src/intravenous/graph_jit/lowering.cpp#L361) |
| 646 | `realize_realtime_port_state_transition_storage` | [realtime_port_state_transition.cpp:2781](../src/intravenous/graph_jit/realtime_port_state_transition.cpp#L2781) |
| 569 | `plan_port_atoms` | [port_group_plan.cpp:293](../src/intravenous/graph_jit/connections/port_group_plan.cpp#L293) |
| 554 | `BackgroundStorageRealization::make_tick_materialization_snapshot` | [background_storage_realization.cpp:2623](../src/intravenous/execution/background/background_storage_realization.cpp#L2623) |
| 550 | `GraphJit::Impl::compile_impl` | [graph_jit.cpp:2015](../src/intravenous/graph_jit/graph_jit.cpp#L2015) |
| 549 | `emit_region_specialization` | [region_specialization_emit.cpp:235](../src/intravenous/graph_jit/region_specialization_emit.cpp#L235) |
| 531 | `plan_sample_realtime_port_state_realizations` | [lowering_plan.cpp:1251](../src/intravenous/graph_jit/lowering_plan.cpp#L1251) |
| 517 | `build_lowering_plan` | [lowering_plan.cpp:3700](../src/intravenous/graph_jit/lowering_plan.cpp#L3700) |
| 488 | `parse_socket_rpc_request` | [socket_rpc_requests.cpp:256](../src/intravenous/rpc/socket_rpc_requests.cpp#L256) |
| 486 | `plan_execution` | [lowering_plan.cpp:2651](../src/intravenous/graph_jit/lowering_plan.cpp#L2651) |
| 461 | `SocketRpcRouteRegistry::SocketRpcRouteRegistry` | [socket_rpc_routes.cpp:177](../src/intravenous/rpc/socket_rpc_routes.cpp#L177) |
| 460 | `execute_realtime_port_state_transfers` | [realtime_port_state_transition.cpp:3496](../src/intravenous/graph_jit/realtime_port_state_transition.cpp#L3496) |
| 456 | `build_structural_sample_routes` | [connection_plan.cpp:745](../src/intravenous/graph_jit/connection_plan.cpp#L745) |
| 422 | `build_steady_storage_transfers` | [realtime_port_state_transition.cpp:1122](../src/intravenous/graph_jit/realtime_port_state_transition.cpp#L1122) |
| 418 | `validate_prepared_transitions` | [realtime_port_state_transition.cpp:2087](../src/intravenous/graph_jit/realtime_port_state_transition.cpp#L2087) |
| 399 | `ModuleLoader::Impl::load_compiled_package` | [loader.cpp:1195](../src/intravenous/module/loader.cpp#L1195) |
| 389 | `NodeInstances::apply_project_graph_transaction` | [node_instances.cpp:1561](../src/intravenous/project/node_instances.cpp#L1561) |
| 379 | `plan_event_realtime_port_state_realizations` | [lowering_plan.cpp:1815](../src/intravenous/graph_jit/lowering_plan.cpp#L1815) |
| 371 | `realtime_ready_group_batches` | [lowering_plan.cpp:3232](../src/intravenous/graph_jit/lowering_plan.cpp#L3232) |
| 355 | `BackgroundPropagationWorkspace::prepare` | [background_coverage_propagation.cpp:2337](../src/intravenous/execution/background/background_coverage_propagation.cpp#L2337) |
| 348 | `TickInvocationWorkspace::Impl::Impl` | [tick_invocation_frame.cpp:958](../src/intravenous/execution/tick_invocation_frame.cpp#L958) |
| 341 | `load_metadata` | [iv_package_finalize.cpp:697](../package_tools/iv_package_finalize.cpp#L697) |
| 341 | `validate_background_runtime_plan` | [background_storage_plan.cpp:1461](../src/intravenous/graph_jit/background_storage_plan.cpp#L1461) |
| 323 | `RegionSpecializationCompiler::compile` | [graph_jit.cpp:1503](../src/intravenous/graph_jit/graph_jit.cpp#L1503) |
| 323 | `ModuleLoaderPackages.RegisteredNodeAndModuleUseProviderConstructionArguments` | [module_loader_failures_gtest.cpp:733](../tests/module_loader_failures_gtest.cpp#L733) |
| 320 | `BackgroundStorageRealization::BackgroundStorageRealization` | [background_storage_realization.cpp:1102](../src/intravenous/execution/background/background_storage_realization.cpp#L1102) |
| 315 | `ProjectGraph::rebuild_locked` | [project_graph.cpp:1592](../src/intravenous/project/project_graph.cpp#L1592) |
| 315 | `GraphJitRuntimeFixture.GeneratedRegionAndCoverageCallbacksPreserveAuthoredRegions` | [background_gtest.cpp:625](../tests/modules/graph_jit/background_gtest.cpp#L625) |
| 308 | `createAutomationCurvePresentation` | [automationCurvePresentation.ts:86](../vscode/client/src/automationCurvePresentation.ts#L86) |
| 302 | `make_replacement_snapshot` | [project_graph_gtest.cpp:639](../tests/modules/project_graph_gtest.cpp#L639) |
| 301 | `layout_event_ports` | [event_port_plan.cpp:3283](../src/intravenous/graph_jit/lowering/event_port_plan.cpp#L3283) |
| 299 | `BackgroundEvaluationTransaction::Impl::run` | [background_evaluation_transaction.cpp:1670](../src/intravenous/execution/background/background_evaluation_transaction.cpp#L1670) |
| 286 | `GraphEditorProvider::attach` | [graphEditorProvider.ts:484](../vscode/client/src/graphEditorProvider.ts#L484) |
| 282 | `ProjectGraph::handle_socket_rpc_get_graph_scope` | [project_graph.cpp:3918](../src/intravenous/project/project_graph.cpp#L3918) |
| 280 | `normalize_sample_target_projections` | [connection_plan.cpp:1509](../src/intravenous/graph_jit/connection_plan.cpp#L1509) |
| 275 | `capture_inputs` | [graph_jit.cpp:195](../src/intravenous/graph_jit/graph_jit.cpp#L195) |
| 274 | `finalize_background_runtime_plan` | [background_storage_plan.cpp:1809](../src/intravenous/graph_jit/background_storage_plan.cpp#L1809) |
| 272 | `inventory_sample_connections` | [connection_plan.cpp:1230](../src/intravenous/graph_jit/connection_plan.cpp#L1230) |
| 264 | `plan_package_imports` | [lowering_plan.cpp:2254](../src/intravenous/graph_jit/lowering_plan.cpp#L2254) |
| 262 | `RootPlanBuilder::build_binding_template` | [root_plan.cpp:1608](../src/intravenous/graph_jit/realtime/root_plan.cpp#L1608) |
| 261 | `ModuleLoader::Impl::build` | [loader.cpp:853](../src/intravenous/module/loader.cpp#L853) |
| 257 | `main` | [iv_edit_latency_benchmark.cpp:1038](../benchmark/iv_edit_latency_benchmark.cpp#L1038) |
| 255 | `BackgroundStorageRealization.FreezesTickVisibleStorageAndDefersPinnedSnapshotReclamation` | [background_storage_realization_gtest.cpp:497](../tests/background_storage_realization_gtest.cpp#L497) |
| 254 | `schedule_background_topology` | [connection_plan.cpp:3978](../src/intravenous/graph_jit/connection_plan.cpp#L3978) |
| 249 | `ProjectPersistence::replay` | [project_persistence.cpp:258](../src/intravenous/project/persistence/project_persistence.cpp#L258) |
| 247 | `construct_value` | [configuration_expression.cpp:545](../src/intravenous/project/configuration_expression.cpp#L545) |
| 241 | `plan_event_groups` | [port_group_plan.cpp:1085](../src/intravenous/graph_jit/connections/port_group_plan.cpp#L1085) |
| 240 | `NodeInstances::configure_and_embed` | [node_instances.cpp:1048](../src/intravenous/project/node_instances.cpp#L1048) |
| 236 | `build_structural_event_routes` | [connection_plan.cpp:1917](../src/intravenous/graph_jit/connection_plan.cpp#L1917) |
| 225 | `inject_package_configuration_metadata` | [iv_package_finalize.cpp:1992](../package_tools/iv_package_finalize.cpp#L1992) |
| 222 | `ProjectGraph::compile_graph` | [project_graph.cpp:720](../src/intravenous/project/project_graph.cpp#L720) |
| 218 | `build_semantic_background_plan` | [connection_plan.cpp:2727](../src/intravenous/graph_jit/connection_plan.cpp#L2727) |
| 215 | `BackgroundStorageRealization::stage_persisted_data` | [background_storage_realization.cpp:3183](../src/intravenous/execution/background/background_storage_realization.cpp#L3183) |
| 215 | `IvModuleSourceIntrospection::query_by_spans` | [iv_module_source_introspection.cpp:421](../src/intravenous/packages/iv_module_source_introspection.cpp#L421) |
| 214 | `validate_tick_runtime_plan` | [background_storage_plan.cpp:2094](../src/intravenous/graph_jit/background_storage_plan.cpp#L2094) |
| 213 | `execute_realtime_root` | [realtime_root.cpp:2233](../src/intravenous/graph_jit/realtime_root.cpp#L2233) |
| 210 | `BackgroundEvaluationTransaction::Impl::select_storage_regions` | [background_evaluation_transaction.cpp:960](../src/intravenous/execution/background/background_evaluation_transaction.cpp#L960) |
| 208 | `inventory_event_connections` | [connection_plan.cpp:2163](../src/intravenous/graph_jit/connection_plan.cpp#L2163) |
| 206 | `BackgroundEvaluationCallFrame.OwnsOperationFramesAndTockBindings` | [background_storage_realization_gtest.cpp:2300](../tests/background_storage_realization_gtest.cpp#L2300) |
| 205 | `NodeInstances::configure_cached` | [node_instances.cpp:651](../src/intravenous/project/node_instances.cpp#L651) |
| 205 | `ProjectGraph::handle_observation_request` | [project_graph.cpp:4210](../src/intravenous/project/project_graph.cpp#L4210) |
| 202 | `deserialize_binary_configured_graph` | [configured_graph_binary_archive.hpp:1013](../src/intravenous/module/configured_graph_binary_archive.hpp#L1013) |
| 201 | `finalize_tick_runtime_plan` | [background_storage_plan.cpp:2420](../src/intravenous/graph_jit/background_storage_plan.cpp#L2420) |
| 199 | `GraphJitRuntimeFixture.PersistedTickRecordingsFeedPlanarSampleAndEventCoverageInputs` | [background_gtest.cpp:1375](../tests/modules/graph_jit/background_gtest.cpp#L1375) |
| 193 | `run` | [iv_playback_benchmark.cpp:238](../benchmark/iv_playback_benchmark.cpp#L238) |
| 192 | `ConfigurationTypeIdentityCollector::schema_metadata` | [iv_clang_source_introspection_plugin.cpp:1762](../package_tools/iv_clang_source_introspection_plugin.cpp#L1762) |
| 191 | `ProjectGraph::compile_worker` | [project_graph.cpp:1085](../src/intravenous/project/project_graph.cpp#L1085) |
| 190 | `ProjectGraph::apply_edit_locked` | [project_graph.cpp:2667](../src/intravenous/project/project_graph.cpp#L2667) |
| 190 | `GraphJitSharedRuntimeFixture.BuildPackage` | [runtime_gtest.cpp:261](../tests/modules/graph_jit/runtime_gtest.cpp#L261) |
| 189 | `NodeDefinitions::handle_package_definitions_publication` | [node_definitions.cpp:101](../src/intravenous/packages/node_definitions.cpp#L101) |
| 188 | `GraphJitRuntimeFixture.DynamicManualControlInterpolatesAcrossAllPrimitiveSlices` | [manual_control_gtest.cpp:231](../tests/modules/graph_jit/manual_control_gtest.cpp#L231) |
| 187 | `BackgroundStorageRealization::seal` | [background_storage_realization.cpp:1544](../src/intravenous/execution/background/background_storage_realization.cpp#L1544) |
| 187 | `GraphJitSampleStoragePlan.DetachedCompositionUsesPersistentShiftedTimeline` | [storage_plan_gtest.cpp:1175](../tests/modules/graph_jit/storage_plan_gtest.cpp#L1175) |
| 186 | `plan_sample_groups` | [port_group_plan.cpp:879](../src/intravenous/graph_jit/connections/port_group_plan.cpp#L879) |
| 183 | `applyMessage` | [GraphEditor.vue:1142](../vscode/client/webview/graph/GraphEditor.vue#L1142) |
| 182 | `BackgroundEvaluationCallFrame.ReservesAndValidatesReplaySchedule` | [background_storage_realization_gtest.cpp:2759](../tests/background_storage_realization_gtest.cpp#L2759) |
| 182 | `GraphJitRuntimeFixture.CyclicRegionSpecializationMatchesInterpreterAndIsReused` | [specialization_gtest.cpp:96](../tests/modules/graph_jit/specialization_gtest.cpp#L96) |
| 180 | `expand_output_gains` | [output_gain.h:110](../src/intravenous/graph_jit/output_gain.h#L110) |
| 179 | `RootPlanBuilder::build_tick_slots` | [root_plan.cpp:1200](../src/intravenous/graph_jit/realtime/root_plan.cpp#L1200) |
| 178 | `run` | [headlessClient.ts:71](../vscode/client/src/headlessClient.ts#L71) |
| 175 | `expect_latent_sample_window_survives_cutover` | [transition_gtest.cpp:331](../tests/modules/graph_jit/transition_gtest.cpp#L331) |
| 174 | `ProjectGraph::dispatch_node_message_locked` | [project_graph.cpp:2088](../src/intravenous/project/project_graph.cpp#L2088) |
| 173 | `inventory_sequential_input_in_flight_values` | [connection_plan.cpp:4414](../src/intravenous/graph_jit/connection_plan.cpp#L4414) |
| 173 | `RootPlanBuilder::composition_write` | [root_plan.cpp:564](../src/intravenous/graph_jit/realtime/root_plan.cpp#L564) |
| 173 | `activate` | [extension.ts:127](../vscode/client/src/extension.ts#L127) |
| 172 | `run_server_mode` | [app.cpp:314](../src/intravenous/app/app.cpp#L314) |
| 172 | `configure_manual_controls` | [project_graph.cpp:1414](../src/intravenous/project/project_graph.cpp#L1414) |
| 171 | `finalize_schedule` | [realtime_schedule_plan.cpp:133](../src/intravenous/graph_jit/connections/realtime_schedule_plan.cpp#L133) |
| 171 | `GraphJitConnectionPlan.RetainsCompleteBackgroundTopologyAndRetention` | [graph_jit_connection_plan_gtest.cpp:2953](../tests/graph_jit_connection_plan_gtest.cpp#L2953) |
| 170 | `BackgroundCoverageState::prepare_migration_from` | [background_coverage_propagation.cpp:2017](../src/intravenous/execution/background/background_coverage_propagation.cpp#L2017) |

### Files over 1,000 lines

The list stops at 35 entries. File counts include blank lines and comments.

| Lines | File |
| ---: | --- |
| 5,275 | [`src/intravenous/graph_jit/connection_plan.cpp`](../src/intravenous/graph_jit/connection_plan.cpp) |
| 5,070 | [`tests/graph_jit_connection_plan_gtest.cpp`](../tests/graph_jit_connection_plan_gtest.cpp) |
| 5,028 | [`tests/modules/project_graph_gtest.cpp`](../tests/modules/project_graph_gtest.cpp) |
| 4,841 | [`tests/graph_executor_gtest.cpp`](../tests/graph_executor_gtest.cpp) |
| 4,417 | [`src/intravenous/project/project_graph.cpp`](../src/intravenous/project/project_graph.cpp) |
| 4,314 | [`src/intravenous/graph_jit/lowering_plan.cpp`](../src/intravenous/graph_jit/lowering_plan.cpp) |
| 4,044 | [`src/intravenous/execution/storage/persisted_data_store.cpp`](../src/intravenous/execution/storage/persisted_data_store.cpp) |
| 3,965 | [`src/intravenous/graph_jit/realtime_port_state_transition.cpp`](../src/intravenous/graph_jit/realtime_port_state_transition.cpp) |
| 3,690 | [`src/intravenous/graph_jit/sample_storage_plan.cpp`](../src/intravenous/graph_jit/sample_storage_plan.cpp) |
| 3,660 | [`src/intravenous/graph_jit/lowering/event_port_plan.cpp`](../src/intravenous/graph_jit/lowering/event_port_plan.cpp) |
| 3,608 | [`tests/persisted_data_store_gtest.cpp`](../tests/persisted_data_store_gtest.cpp) |
| 3,469 | [`src/intravenous/execution/background/background_storage_realization.cpp`](../src/intravenous/execution/background/background_storage_realization.cpp) |
| 3,404 | [`tests/modules/graph_jit/transition_gtest.cpp`](../tests/modules/graph_jit/transition_gtest.cpp) |
| 3,299 | [`src/intravenous/ports.h`](../src/intravenous/ports.h) |
| 3,232 | [`tests/background_storage_realization_gtest.cpp`](../tests/background_storage_realization_gtest.cpp) |
| 3,032 | [`package_tools/iv_package_finalize.cpp`](../package_tools/iv_package_finalize.cpp) |
| 2,933 | [`src/intravenous/execution/graph_executor.cpp`](../src/intravenous/execution/graph_executor.cpp) |
| 2,929 | [`src/intravenous/graph_jit/realtime/root_plan.cpp`](../src/intravenous/graph_jit/realtime/root_plan.cpp) |
| 2,863 | [`src/intravenous/graph_jit/lowering.cpp`](../src/intravenous/graph_jit/lowering.cpp) |
| 2,823 | [`tests/node_build_request_gtest.cpp`](../tests/node_build_request_gtest.cpp) |
| 2,795 | [`package_tools/iv_clang_source_introspection_plugin.cpp`](../package_tools/iv_clang_source_introspection_plugin.cpp) |
| 2,719 | [`src/intravenous/execution/background/background_coverage_propagation.cpp`](../src/intravenous/execution/background/background_coverage_propagation.cpp) |
| 2,713 | [`src/intravenous/graph_jit/graph_jit.cpp`](../src/intravenous/graph_jit/graph_jit.cpp) |
| 2,679 | [`src/intravenous/graph_jit/background_storage_plan.cpp`](../src/intravenous/graph_jit/background_storage_plan.cpp) |
| 2,452 | [`src/intravenous/graph_jit/realtime_root.cpp`](../src/intravenous/graph_jit/realtime_root.cpp) |
| 2,276 | [`tests/modules/graph_jit/background_gtest.cpp`](../tests/modules/graph_jit/background_gtest.cpp) |
| 2,154 | [`src/intravenous/execution/background/background_evaluation_transaction.cpp`](../src/intravenous/execution/background/background_evaluation_transaction.cpp) |
| 2,041 | [`tests/modules/graph_jit/samples_gtest.cpp`](../tests/modules/graph_jit/samples_gtest.cpp) |
| 1,984 | [`src/intravenous/project/node_instances.cpp`](../src/intravenous/project/node_instances.cpp) |
| 1,957 | [`tests/modules/graph_jit/storage_plan_gtest.cpp`](../tests/modules/graph_jit/storage_plan_gtest.cpp) |
| 1,942 | [`src/intravenous/module/loader.cpp`](../src/intravenous/module/loader.cpp) |
| 1,906 | [`src/intravenous/execution/background/background_evaluation_call_frame.cpp`](../src/intravenous/execution/background/background_evaluation_call_frame.cpp) |
| 1,849 | [`src/intravenous/graph_jit/lowering/sample_port_plan.cpp`](../src/intravenous/graph_jit/lowering/sample_port_plan.cpp) |
| 1,833 | [`tests/modules/graph_jit/events_gtest.cpp`](../tests/modules/graph_jit/events_gtest.cpp) |
| 1,780 | [`src/intravenous/continuous_value.h`](../src/intravenous/continuous_value.h) |
