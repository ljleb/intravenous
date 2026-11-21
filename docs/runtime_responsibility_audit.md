# Runtime responsibility audit

This implementation audit began on 2026-11-21. It covers project-owned
source and test files above 4,000 lines and related work in `Observations`.
It excludes vendored miniaudio.

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
[Event port planner](../src/intravenous/graph_jit/lowering/event_port_plan.cpp#L3787)

### Stack-budget retries

`ConnectionTopology::build_analysis_plan()` prepares one
`ConnectionAnalysisPlan` per callback order. Sample and Event storage
planners share its schedule, delivery routes, and buffer requirements
across trials. Each trial applies the cost model and packs the buffers
to measure the stack usage.

GraphJit plans package imports from `ConnectionTopology` before the
trials. It fills in replay metadata, Event capture capacities, and
port-state transfer records after it accepts the schedule and layout.

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

### Event conversion sharing

Staged fan-in, ordinary fan-in, single-source delivery, and feedback each
search `EventPortBindingPlan::materializations` for a conversion to reuse.
Each path also widens the history and cost bounds of a shared conversion.
The matching rules differ. For example, fan-in checks whether the target
keeps source indices. Some single-source and feedback paths omit that
check. Review the matching rules and give conversion sharing one owner. Its inputs include the conversion, execution scope,
source filter, and whether the result keeps source indices.
[Staged fan-in](../src/intravenous/graph_jit/lowering/event_port_plan.cpp#L1825) ·
[Ordinary fan-in](../src/intravenous/graph_jit/lowering/event_port_plan.cpp#L2166) ·
[Single-source delivery](../src/intravenous/graph_jit/lowering/event_port_plan.cpp#L2541) ·
[Feedback](../src/intravenous/graph_jit/lowering/event_port_plan.cpp#L3016)

### Connection routing, scheduling, and port grouping

`ConnectionTopology::build()` owns effective routing and authored
validation. `build_analysis_plan()` orders callbacks, binds `detach()`
regions, computes latency compensation, and groups ports for storage.
The proposed split extracts scheduling and compensation together first.
A second extraction groups `plan_port_atoms()`, `plan_sample_groups()`,
and `plan_event_groups()` in a file that owns port grouping.
[Connection analysis](../src/intravenous/graph_jit/connection_plan.cpp#L7007)

### Realtime root construction and execution

`RootPlanBuilder` resolves bindings and validates the plan before
publication. `execute_realtime_root()` consumes the completed
`RealtimeRootPlan` on the audio thread. That plan gives the two files
a boundary. Some batch checks can share a rule, such as which operations
force scalar execution. Checks that compare resolved callback addresses
belong in construction because the addresses are available there.
[Root construction](../src/intravenous/graph_jit/realtime_root.cpp#L2871) ·
[Batch validation](../src/intravenous/graph_jit/realtime_root.cpp#L4839) ·
[Root execution](../src/intravenous/graph_jit/realtime_root.cpp#L5538)

The batch overlap check uses the packed layout. If the check rejects a
batch, GraphJit builds the declarations again for the new order.
[Batch selection](../src/intravenous/graph_jit/lowering_plan.cpp#L5954)

### Ownership in the large source files

| File | Lines | Boundary to scrutinize |
| --- | ---: | --- |
| [`connection_plan.cpp`](../src/intravenous/graph_jit/connection_plan.cpp#L7007) | 7,286 | `ConnectionTopology::build_analysis_plan()` turns effective routes into a realtime schedule, derives compensation and in-flight requirements, and records background bindings for the schedule. |
| [`lowering_plan.cpp`](../src/intravenous/graph_jit/lowering_plan.cpp#L5528) | 6,132 | `build_lowering_plan()` coordinates connection analysis, storage choice, package imports and scheduling. The separate Event planner keeps stream binding, storage selection, and buffer packing together. Storage trials share one connection analysis per callback order. |
| [`realtime_root.cpp`](../src/intravenous/graph_jit/realtime_root.cpp#L2871) | 5,757 | `RootPlanBuilder::build()` validates addresses and materializes primitive bindings, ready groups and region specializations. `execute_realtime_root()` consumes a completed `RealtimeRootPlan` on the audio thread. Use that plan as the boundary between cold binding and realtime execution, including the layout checks that the executor can then rely on. |
| [`project_graph.cpp`](../src/intravenous/project/project_graph.cpp#L1558) | 4,390 | `rebuild_locked()` handles authored edits, calls instance and connection configuration, and publishes a snapshot. `configure_manual_controls()` now owns control binding on a finished `ConfiguredGraph`. Read-model projection, edit history, and executor preparation can consume that result. |
| [`persisted_data_store.cpp`](../src/intravenous/execution/storage/persisted_data_store.cpp#L3088) | 4,044 | `Candidate::overwrite()` and the Region replacement functions encode snapshot and recording semantics. The cache policy consumes payload costs and usage facts. The store owns payload estimates, lease release, and recording writes. |

The [connection-planning tests](../tests/graph_jit_connection_plan_gtest.cpp)
(4,988 lines), [project graph tests](../tests/modules/project_graph_gtest.cpp)
(4,996 lines), and [executor tests](../tests/graph_executor_gtest.cpp)
(4,841 lines) can follow production boundaries as they become explicit.
Useful divisions include authored routing versus scheduling, project edits
versus execution publication, and cutover versus background Coverage.

### Cross-component header dependencies

The move gives the components separate locations. Their headers still
expose some cross-component dependencies. For example, `observations.h`
includes the RPC request header for a disconnect record. A client identity
record shared at the event boundary can remove that dependency.
`graph_jit.h` also defines the request that serves the `ProjectGraph`
bridge. Moving that request to the bridge boundary can keep the compiler
interface focused on compilation inputs and results.
