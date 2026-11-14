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

- [Lane Knobs Before Default Inline Display (2026-11-14)](./inline_lane_knobs_before_default_display_2026-11-14.md)
- [Knob Reset Before Double-Click (2026-11-14)](./knob_reset_before_double_click_2026-11-14.md)
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


## Superseded transition and compiler design

- [Manual-Control Debounce Before Hover Settling (2026-11-14)](./manual_control_debounce_before_hover_settling_2026-11-14.md)
- [Application Modules And Batched Event Architecture](./application_modules_event_architecture.md)
- [Unified Graph Direction](./unified_graph_direction.md)
- [LLVM Hot Reload And Whole-Graph Design](./intravenous-llvm-hot-reload-and-whole-graph-design.md)
- [Modular Builder-Lowering Pipeline](./builder_lowering_pipeline_design.md)
- [GraphBuilder Historical Decomposition Notes](./graph_builder.md)
- [Representation Migration Inventory](./representation_migration_inventory.md)
- [Runtime Graph Container And constexpr Legacy Audit](./runtime_graph_container_cleanup.md)
- [DSP Execution And Storage Terminology Migration](./dsp_execution_storage_terminology_migration.md)
- [Virtual Nodes And Channel-Aware DSP Graph Direction](./virtual_nodes_channel_aware_graph_direction.md)
- [Project Persistence And Command Surface Direction](./project_persistence_command_surface_direction.md)

## Superseded lane-query, visualization, and VS Code UI design

- [Lane Filters Query Design](./lane_filters_query_design.md)
- [Lane Query Completion Plan](./lane_query_completion_plan.md)
- [Lane Query Tooling Direction](./lane_query_tooling_direction.md)
- [Lane View Layout Persistence Note](./lane_view_layout_persistence_note.md)
- [Lanes Visualization Direction](./lanes_visualization_direction.md)
- [VS Code UI Architecture Direction](./vscode_ui_architecture_direction.md)
- [VS Code UI Architecture Implementation Audit](./vscode_ui_architecture_implementation_audit.md)
- [VS Code UI Implementation Phases](./vscode_ui_implementation_phases.md)
- [Historical Modules Follow-Up Notes](./modules_follow_up_notes_lane_era.md)
- [Historical Runtime PGO Direction (module-era snapshot)](./iv_module_runtime_pgo_direction_module_era.md)

## Pre-cleanup snapshots of current documents

- [September 2026 pre-big-rename snapshots](./pre_big_rename/README.md) preserve verbatim copies of current-design documents before the terminology/dead-code cleanup. Use them for rationale/history only; the corresponding files in `docs/` are normative.

## Superseded project session client behavior

- [Project Session Client Attachment Before The Owned-Server Default (2026-11-12)](./project_session_client_attachment_before_owned_server_2026-11-12.md) preserves the rule that a client discovers and attaches to an existing server and never owns the one it starts; the replacement default is in [Project Session Server Direction](../project_session_server_direction.md).

## Superseded Vue observation design

- [Vue Observation Rebinding Before The Realization-Input Invariant (2026-11-12)](./vue_observation_rebinding_before_realization_input_invariant_2026-11-12.md) preserves the after-cutover rebinding wording that left structural edits free to drop probes; the replacement is the realization-input invariant in [Vue Output Observation](../vue_output_observation_direction.md) and [Project Graph Application Architecture](../project_graph_application_architecture.md).

- [Vue Host And Observation Integration Before The 2026-11-12 Clarifications](./vue_host_and_observation_integration_before_2026-11-12_clarifications.md) preserves the replaced registration-removal wording and broad GraphJit observation description; see the current [Node Presentation](../node_presentation_and_manual_controls_direction.md), [Vue Output Observation](../vue_output_observation_direction.md), and [Graph JIT](../graph_jit_direction.md) directions.

- [Vue Observation Before Output Queries And `Observations` Ownership (2026-11-12)](./vue_output_observation_before_module_and_range_direction_2026-11-12.md) retains the original superseded normative passages; the replacement contract is [Vue Output Observation And Query Direction](../vue_output_observation_direction.md).

## Completed migrations, experiments, and checkpoints

- [Vue Connection Display Before Lane Selection (2026-11-13)](./vue_connection_display_before_lane_selection_2026-11-13.md) preserves the temporary-wire-only checkpoint before selected lanes gained connection display and grouped-slot expansion.

- [Vue Port Actions Before Pointer Capture (2026-11-13)](./vue_port_actions_before_pointer_capture_2026-11-13.md) preserves the toolbar/lane-button checkpoint and native-drag implementation before the owner's interaction refinements.

- [Scope Interface Availability Before Failure-Only Construction (2026-11-13)](./scope_interface_availability_before_failure_only_2026-11-13.md) preserves the missing-port defect and configuration alternatives. The owner selected extra construction only after a failed build.

- [Playback Discussion Before The Initial Contract (2026-11-13)](./playback_design_discussion_before_contract_2026-11-13.md) preserves the initial coordination note. The playback contract and proposed output-derived grids are in [Project Playback And Presentation Grids](../project_playback_and_presentation_grids_direction.md).

- [Event Source Marking Before Event Source Scopes (2026-11-12)](./event_source_marking_before_event_source_scopes_2026-11-12.md) preserves the single-raise `IV_INVOKE_LINKER_EVENT_SOURCE` primitive and its unchecked top-level raises; the replacement is `IV_EVENT_SOURCE` in [Event Propagation Tree Constraint](../event_propagation_tree_constraint.md).

- [Event Flows Before Root-Batched Publication (2026-11-12)](./event_flows_before_root_batched_publication_2026-11-12.md) preserves the implemented propagation trees in which modules notified `SocketRpcServer` partway through a cause; the replacement trees are in [Event Flows](../event_flows/README.md).

- [Node State And Lifecycle Before Tick/Tock Authored-State Unification (2026-11-12)](./node_state_lifecycle_before_unification_2026-11-12.md)

- [Presentation And Live Observation Direction Before The 2026-11-12 Vue Revision](./node_presentation_and_live_observation_before_vue_direction_2026-11-12.md)

- [Front 16 Region Specialization Before The First Profile Correction, 2026-11-11](./front_16_region_specialization_before_first_profile_correction_2026-11-11.md)
- [Front 14 Presentation Contract Before Concrete Records, 2026-11-04](./front_14_presentation_contract_before_concrete_records_2026-11-04.md)
- [Front 14 Manual-Control JIT Policy Before Demand-Driven Activation, 2026-11-04](./front_14_manual_control_jit_policy_before_demand_activation_2026-11-04.md)
- [Front 13 Backend TypeScript Generation Before the Language-Neutral Protocol, 2026-11-03](./front_13_backend_typescript_generation_before_neutral_protocol_2026-11-03.md)
- [Front 13 Dynamic Module-Message Registration Before Class Modules, 2026-11-03](./front_13_dynamic_module_message_registration_2026-11-03.md)
- [Front 13 Predecessor Argument Introspection False Start, 2026-11-03](./front_13_predecessor_argument_introspection_false_start_2026-11-03.md)
- [Front 9 Status Before Implementation, 2026-11-02](./front_9_status_before_implementation_2026-11-02.md)
- [Front 9 Reference Syntax Before 2026-11-02](./front_9_configuration_reference_syntax_before_2026-11-02.md)
- [Front 10 Pre-Implementation Checkpoint, 2026-11-02](./front_10_pre_implementation_checkpoint_2026-11-02.md)
- [Front 11 Pre-Implementation Checkpoint, 2026-11-02](./front_11_pre_implementation_checkpoint_2026-11-02.md)
- [Source Introspection With Fake Live Port Values](./source_introspection_fake_live_port_values.md)
- [Superseded Linear And Saturating Global Index Semantics](./linear_global_index_semantics.md)
- [Superseded Temporary-Adapter Coordination Policy](./implementation_fronts_temporary_adapter_policy.md)
- [Historical Sample-Only Port Configuration Sketch](./sample_port_config_before_typed_continuous_api.md)
- [Port-realization Status Before Typed Authored Port Configs](./port_realization_status_before_typed_port_configs.md)
- [Port-realization Status Before Type-Owned Tick Execution](./port_realization_status_before_type_owned_tick_execution.md)
- [Two-Form Port Schema Before Region/Coverage](./two_form_port_schema_before_region_coverage.md)
- [Superseded Uniform FFT Neutral Fill](./fft_uniform_neutral_fill.md)
- [Superseded Rational Port Pace And SDF Scheduling Direction](./rational_port_pace_and_sdf_scheduling.md)
- [Superseded Variable FFTBlock Spectrum Layout](./variable_fftblock_spectrum_layout.md)
- [Superseded Separate Dyadic Execution Index](./separate_dyadic_execution_index.md)
- [Superseded Global Timeline As Sequential Storage Address](./global_timeline_as_sequential_storage_address.md)
- [Superseded Core-Owned FFTW Backend Proposal](./core_owned_fftw_backend_proposal.md)
- [LLVM Module Reload Migration Log](./llvm_module_reload_migration.md)
- [Module Builder Migration](./module_builder_migration.md)
- [Phase 4 constexpr migration scope](./phase4_constexpr_migration_scope.md)
- [IV module compile-time profiling record](./iv_sample_lowering_complexity.md)
- [Last Two Commits Testing Plan](./last_two_commits_testing_plan.md)

## Vue Stage 2 project-root canvas checkpoint

- [Startup Status Before Classified Scope State](./vue_startup_status_before_classified_scope_state_2026-11-12.md)
  preserves the startup diagnostic investigation and the initial unbound viewport.
- [Lane Layout Before Fixed Vertical Zoom And Source Selection](./vue_lane_layout_before_fixed_zoom_and_selection_2026-11-12.md)
  preserves the fit-to-height implementation and the incorrect filtering prohibition.
- [Primary Timeline Before The Default Lane](./vue_timeline_before_default_lane_2026-11-12.md)
  records the status text and per-child time grid replaced by the default lane.
- [Vue Stage 2 Before The Primary Timeline](./vue_stage_2_before_primary_timeline_2026-11-12.md)
  preserves the canvas status before the confirmed primary timeline layout.
- [Vue Stage 2 Before The Project Graph Canvas](./vue_stage_2_before_project_graph_canvas_2026-11-12.md)
  preserves the implementation status before the project-root editor was written.

- [Connection Gestures Before The Shift-Only Chooser](./vue_connection_gestures_before_shift_only_chooser_2026-11-13.md)
  preserves the proposed right-click menu and the checkpoint before Shift-only
  connection pickup, disconnection, and endpoint movement.

- [Vue Chooser Before Held Shift And Vertical Curves](./vue_chooser_before_held_shift_and_vertical_curves_2026-11-13.md)
  preserves the tap-to-open chooser checkpoint and the previous horizontal
  wire endpoint tangents.

- [Vue Ports Before Single-Connection Choosers](./vue_ports_before_single_connection_choosers_2026-11-13.md)
  preserves the fit-based grouping proposal, two-direct-port checkpoint, and
  chooser placement before its first entry replaced the hovered port circle.

- [Vue Port Lists Before First-Item Alignment](./vue_port_lists_before_first_item_alignment_2026-11-13.md)
  preserves grouped-icon caps and connection summaries before ordinary port
  lists adopted the same first-item alignment as connection choosers.

- [Vue Shift Before Shared Modifier State](./vue_shift_before_shared_modifier_state_2026-11-13.md)
  preserves per-event modifier handling before port lists shared the editor's
  Shift state across nested hover transitions.

- [Vue Wires Before The Single Cubic Rule](./vue_wires_before_single_cubic_rule_2026-11-13.md)
  preserves self-connection detours, hover-based colour overrides, and
  colour-dependent cursor tangents before drawing was simplified.

- [Vue Port Targets Before Muted Unavailable Ports](./vue_port_targets_before_muted_unavailable_ports_2026-11-13.md)
  preserves candidate rings and automatic opposite-side popovers before
  unavailable ports were muted and destination hover was restricted to its own side.

- [Vue Port Muting Before Destination-Side Feedback](./vue_port_muting_before_destination_side_only_2026-11-13.md)
  preserves the broader muting rule and the first-press focus-order attempt
  before the owner confirmed that native window activation still failed.

- [Vue Port Grab Before Native Window Focus Reporting](./vue_port_grab_before_native_window_focus_2026-11-13.md)
  preserves the unresolved recorder status and the owner's trace that identified
  webview blur as the cancellation source after an accepted first press.

- [Removed Port Grab Recorder](./vue_port_grab_recorder_removed_2026-11-13.md)
  preserves the temporary JSON recording command after its trace led to the
  confirmed native-window focus fix.

- [Undo/Redo Direction Before The First Implementation](./project_undo_redo_before_first_implementation_2026-11-13.md)
  retains the accepted contract and original unimplemented status before the
  first server-owned session history was added.

- [Project History Before Durable Settings](./project_history_before_settings_2026-11-13.md)
  preserves the initial graph-history scope before audio selections and persisted
  toolchain settings joined the same session history.

- [Node-reference Output Selection Before Mixed Outputs](./node_ref_output_selection_before_mixed_outputs_2026-11-14.md)
  preserves the exactly-one-output rules before first-output selection and mixed
  sample/event public declarations were accepted.

- [Knob Recording Before Passive Failure Capture](./knob_pointer_recording_before_passive_failures_2026-11-14.md)
  preserves the recorder procedure that stopped and cancelled a drag at its first
  lock diagnostic, before diagnostics became passive through mouse release.

- [Patched Editor Before an Isolated Profile](./patched_editor_before_isolated_profile_2026-11-14.md)
  preserves the launch procedure that required closing every VSCodium window
  before the optional host backport received its own user-data directory.

- [Retired VSCodium Pointer-Lock Host Workaround](./vscodium_pointer_lock_host_workaround_2026-11-14.md)
  preserves the editor investigation, temporary patch and installation procedures,
  and superseded implementation status after the owner installed the fixed release
  and removed editor tooling from the repository.
