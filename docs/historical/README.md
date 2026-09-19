# Historical Documentation

Everything in this directory is non-normative historical material.

These documents are retained because they record deleted architectures, superseded design directions, completed migrations, profiling experiments, or implementation checkpoints that may still explain why the repository looks the way it does. They must not be treated as current requirements when they conflict with documents in the parent [`docs/`](../README.md) directory.

## Superseded application and execution architecture

- [Application Module Cleanup Direction](./application_module_cleanup_direction.md)
- [Execution Model Direction](./execution_model_direction.md)
- [Execution Recovery Blueprint](./execution_recovery_blueprint.md)
- [Task Runner Execution Direction](./task_runner_execution_direction.md)
- [Timeline Lanes Design Notes](./timeline_lanes_design.md)
- [Graph I/O Repatching Direction](./graph_input_repatching_direction.md)
- [Iv-Module Instance Management Direction](./iv_module_instance_management_direction.md)
- [Iv-Module Instances And Graph-Input Direction](./iv_module_instances_graph_input_direction.md)
- [Instance-Aware Span Query Direction](./instance_aware_span_query_direction.md)

## Superseded lane-era feature designs

- [Audio Device Lanes Direction](./audio_device_lanes_direction.md)
- [MIDI Device Lanes Direction](./midi_device_lanes_direction.md)
- [Configured Lane Creation Direction](./configured_lane_creation_direction.md)
- [Lane Filters Runtime Flow](./lane_filters_runtime_flow.md)
- [Lane Node Inputs-Changed Hook](./lane_node_inputs_changed_hook.md)
- [Lane UI Model Direction](./lane_ui_model_direction.md)
- [Per-Realtime-Lane Meter Presentation State](./realtime_lane_meter_presentation_state_plan.md)
- [Sample Lane Channel Layout Direction](./sample_lane_channel_layout_direction.md)
- [Sample Lane Channel Layout Repo Integration](./sample_lane_channel_layout_repo_integration.md)
- [Block Index Reset Note](./block_index_reset_note.md)

## Completed migrations, experiments, and checkpoints

- [LLVM Module Reload Migration Log](./llvm_module_reload_migration.md)
- [Module Builder Migration](./module_builder_migration.md)
- [Phase 4 constexpr migration scope](./phase4_constexpr_migration_scope.md)
- [IV module compile-time profiling record](./iv_sample_lowering_complexity.md)
- [Last Two Commits Testing Plan](./last_two_commits_testing_plan.md)
