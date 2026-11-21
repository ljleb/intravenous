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

## Catalog publication, test fixture, and cache retention

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

## Manual overrides and authored connections

`ConnectionTopology::build()` indexes structural Sample routing once and
shares the Event connection inventory between both views. Each view resolves
Sample routes with its own override rules, because replacing a module input
can change the sources of its downstream routes. Authored validation checks
feedback, `detach()`, replay dependencies, Region inputs, and complete-buffer
inputs. The effective routes drive the background schedule, the realtime
schedule, and physical storage planning.

## Decisions that come after the work

| Current operation | Work that can be avoided | Where the decision belongs |
| --- | --- | --- |
| [`ProjectGraph::compile_graph`](../src/intravenous/project/project_graph.cpp#L712) requests compilation with a predecessor when bindings alone cannot satisfy a probe change. [`GraphJit`](../src/intravenous/graph_jit/graph_jit.cpp#L2234) derives transition inputs before the executor checks if the compiled code fits the active layout. | Compatible observation code can leave a state-transfer plan unused. The attempt-ID change avoids reserving an execution generation in this case. | Check compatibility against the steady lowering result first, then build the transition only when the check calls for a cutover. Reuse the steady result in either path. |
| [`plan_event_ports()`](../src/intravenous/graph_jit/lowering_plan.cpp#L8592) binds the connected streams again if exact operation costs change a storage kind. | The second binding uses the same node inventory. It still rebuilds the connected operations and their callback slots if the first binding used a different raw output alias, carry buffer, or sequence capacity. | Give each operation a logical stream identity and record how its capacity depends on the source streams. Resolve aliases, capacity, and storage after choosing the storage kinds. Keep one record for shared conversions so the new plan counts each conversion once. |
| [`build_lowering_plan`](../src/intravenous/graph_jit/lowering_plan.cpp#L9903) retries `plan_realtime_ports()` when packed stack size exceeds the budget. After building declarations and imports it can reject a batch and restart planning. | Each stack trial repeats parts of connection analysis and configures background replay. A rejected batch discards declarations and imports as well. | Give storage selection stable inputs for a given callback order and reserve replanning for facts that change with that order. Check batch compatibility as soon as the necessary layouts and callback signatures are known. The packed high-water mark may still require a search. |

A failed executor cutover is different. Preparation can become stale against
the active predecessor after a candidate was eligible. The executor needs to
discard that preparation and rebase accepted successors. Likewise, a private
Tock preview can discard derived results while committed Buffered Sequential
recordings stay with `PersistedDataStore`.

## Ownership in the large source files

| File | Lines | Boundary to scrutinize |
| --- | ---: | --- |
| [`lowering_plan.cpp`](../src/intravenous/graph_jit/lowering_plan.cpp#L9615) | 10,287 | `build_lowering_plan()` coordinates connection analysis, storage choice, package imports and scheduling. Event delivery, storage requirements and callback binding have different inputs and can be tested separately. The split between `select_event_storage()` and `layout_event_ports()` makes those responsibilities explicit. The connected-stream bindings still depend on the chosen storage kind. |
| [`connection_plan.cpp`](../src/intravenous/graph_jit/connection_plan.cpp#L7115) | 7,338 | `ConnectionTopology::build_analysis_plan()` turns effective routes into a realtime schedule, derives compensation and in-flight requirements, and selects storage and background bindings. Stack-budget retries still repeat work within this pass. |
| [`realtime_root.cpp`](../src/intravenous/graph_jit/realtime_root.cpp#L2871) | 5,757 | `RootPlanBuilder::build()` validates addresses and materializes primitive bindings, ready groups and region specializations. `execute_realtime_root()` consumes a completed `RealtimeRootPlan` on the audio thread. Use that plan as the boundary between cold binding and realtime execution, including the layout checks that the executor can then rely on. |
| [`project_graph.cpp`](../src/intravenous/project/project_graph.cpp#L1558) | 4,390 | `rebuild_locked()` handles authored edits, calls instance and connection configuration, and publishes a snapshot. `configure_manual_controls()` now owns control binding on a finished `ConfiguredGraph`. Read-model projection, edit history, and executor preparation can consume that result. |
| [`persisted_data_store.cpp`](../src/intravenous/execution/storage/persisted_data_store.cpp#L3088) | 4,044 | `Candidate::overwrite()` and the Region replacement functions encode snapshot and recording semantics. The cache policy consumes payload costs and usage facts. The store owns payload estimates, lease release, and recording writes. |

The [connection-planning tests](../tests/graph_jit_connection_plan_gtest.cpp)
(4,988 lines), [project graph tests](../tests/modules/project_graph_gtest.cpp)
(4,996 lines), and [executor tests](../tests/graph_executor_gtest.cpp)
(4,841 lines) can follow production boundaries as they become explicit.
Useful divisions include authored routing versus scheduling, project edits
versus execution publication, and cutover versus background Coverage.

## Directory layout and dependency boundaries

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

The move gives the components separate locations. Their headers still
expose some cross-component dependencies. For example, `observations.h`
includes the RPC request header for a disconnect record. A client identity
record shared at the event boundary can remove that dependency.
`graph_jit.h` also defines the request that serves the `ProjectGraph`
bridge. Moving that request to the bridge boundary can keep the compiler
interface focused on compilation inputs and results.
