## Execution Recovery Blueprint

This note records what was recoverable from the build tree and staged tests after the execution-side source files were lost.

### Missing Runtime Files Confirmed By Build Metadata

The following files were previously present and compiled:

- `src/intravenous/runtime/timeline_execution.h`
- `src/intravenous/runtime/timeline_execution.cpp`
- `src/intravenous/runtime/timeline_execution_events.h`
- `src/intravenous/runtime/timeline_execution_events.cpp`
- `src/intravenous/runtime/timeline_timeline_execution_bridge.h`
- `src/intravenous/runtime/timeline_timeline_execution_bridge.cpp`
- `src/intravenous/runtime/timeline_execution_task_runner_bridge.h`
- `src/intravenous/runtime/timeline_execution_task_runner_bridge.cpp`
- `src/intravenous/runtime/iv_module_instances_execution.h`
- `src/intravenous/runtime/iv_module_instances_execution.cpp`
- `src/intravenous/runtime/iv_module_instances_execution_events.h`
- `src/intravenous/runtime/iv_module_instances_execution_events.cpp`
- `src/intravenous/runtime/iv_module_instances_iv_module_instances_execution_bridge.h`
- `src/intravenous/runtime/iv_module_instances_iv_module_instances_execution_bridge.cpp`
- `src/intravenous/runtime/iv_module_instances_execution_task_runner_bridge.h`
- `src/intravenous/runtime/iv_module_instances_execution_task_runner_bridge.cpp`
- `src/intravenous/runtime/graph_input_lanes_execution_edges_events.h`
- `src/intravenous/runtime/graph_input_lanes_execution_edges_events.cpp`
- `src/intravenous/runtime/graph_input_lanes_iv_module_instances_execution_bridge.h`
- `src/intravenous/runtime/graph_input_lanes_iv_module_instances_execution_bridge.cpp`

### Recovered Linkage Shape

Build metadata showed:

- `timeline_execution.cpp`, `timeline_timeline_execution_bridge.cpp`, `timeline_execution_task_runner_bridge.cpp`, `iv_module_instances_execution.cpp`, `iv_module_instances_iv_module_instances_execution_bridge.cpp`, `iv_module_instances_execution_task_runner_bridge.cpp`, and `graph_input_lanes_iv_module_instances_execution_bridge.cpp` were all part of `intravenous_tooling_runtime_objects`.
- `timeline_execution_events.cpp`, `iv_module_instances_execution_events.cpp`, and `graph_input_lanes_execution_edges_events.cpp` were separate event object targets, matching the existing linker-event target pattern used elsewhere in runtime.
- Main targets and focused tests linked against those objects, so they were part of the intended runtime, not dead code.

### Recovered Public API From Staged Tests

#### `TimelineExecution`

Staged tests expect:

- `TimelineExecution(size_t block_size)`
- `void synchronize_from_graph(LaneGraph const&)`
- `void handle_timeline_lanes_changed(TimelineLanesChanged const&)`
- `std::vector<LaneId> realtime_sample_output_lanes() const`
- `void execute_realtime_sample_block(size_t start_index)`
- `std::span<Sample const> realtime_sample_block(LaneId lane) const`
- `std::span<TimedEvent const> realtime_event_block(LaneId lane) const`
- `std::vector<Sample> compiled_sample_block(LaneId lane, size_t start_index)`
- `std::vector<TimedEvent> compiled_event_block(LaneId lane, size_t start_index)`

Expected behavior:

- tracks realtime sample output lanes from the timeline graph
- executes realtime knob chains forward
- allows compiled-sample output to feed realtime-sample input
- allows compiled-event output to feed realtime-event input
- caches compiled output regions by requested block region
- invalidates compiled cache when a lane changes

#### `IvModuleInstancesExecution`

Staged tests expect:

- `TaskGraphUpdate handle_instance_builders_changed(IvModuleInstanceBuildersChanged const&)`
- `TaskGraphUpdate handle_graph_input_lanes_dsp_task_dependencies_changed(GraphInputLanesDspTaskDependenciesChanged const&)`

Expected task ids:

- DSP task id for an instance: `iv_module_instance:dsp:<instance_id>`

Expected dependency translation:

- prerequisite lanes become:
  - `timeline:lane:<lane_id>`

#### `GraphInputLanes` DSP Dependency Event

Staged tests expect:

- `GraphInputLanesDspTaskDependenciesChangedEvent`
- payload type `GraphInputLanesDspTaskDependenciesChanged`
- `created_or_updated` entries containing:
  - `instance_id`
  - `prerequisite_lanes`
- `deleted_instance_ids`

### Recovered Task Id Scheme

Recovered from tests:

- timeline lane task ids: `timeline:lane:<numeric_lane_id>`
- iv-module DSP task ids: `iv_module_instance:dsp:<instance_id>`

### Recovered Dependency Direction

Recovered from tests:

- if a DSP graph instance depends on timeline input lanes, the DSP task declares those lane tasks in `depends_on`
- example recovered expected dependencies:
  - `timeline:lane:4`
  - `timeline:lane:8`

### Recovered Architecture Direction

The recovered structure matches the documented direction:

- `Timeline` remains structural
- `TimelineExecution` is a derived execution-side runtime module
- `IvModuleInstancesExecution` is a separate derived execution-side runtime module for DSP graphs
- `GraphInputLanes` owns cross-domain dependency publication for graph inputs
- bridges connect:
  - `Timeline` -> `TimelineExecution`
  - `TimelineExecution` -> `TaskRunner`
  - `IvModuleInstances` -> `IvModuleInstancesExecution`
  - `GraphInputLanes` -> `IvModuleInstancesExecution`
  - `IvModuleInstancesExecution` -> `TaskRunner`

### Important Current Source Gaps

Current source is still on the older model:

- `Timeline` still owns multiple graphs and exposes pull-based realtime sampling
- `lane_node/generate.h` still uses `generate(ctx)` and callback-style realtime inputs
- `LaneGraph` still owns mixed structure and execution concerns
- none of the execution-side runtime files above currently exist in source

### Reconstruction Strategy

The least disruptive way back is:

1. Restore the newer lane-node tick API while keeping compatibility shims for old code where practical.
2. Recreate the missing event and bridge files following the existing linker-event runtime pattern.
3. Recreate `TimelineExecution` and `IvModuleInstancesExecution` to satisfy the staged tests first.
4. Use the staged tests as the authoritative recovered behavioral spec.
