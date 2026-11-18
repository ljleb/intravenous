# Documentation

Documents directly under `docs/` describe the current implementation, the current design direction, or future work that is still compatible with that direction.

The rules for distinguishing normative design, current implementation, implementation
friction, and historical material are defined by
[Design, Implementation, And Documentation Workflow](./development_workflow.md).
Implementation success must not retroactively redefine design intent, and semantic
information removed from a current document must be preserved under `historical/`.

The primary application-architecture entry points are:

- [Outstanding Implementation Fronts](./implementation_fronts_direction.md)
- [Vue Client Implementation Stages](./vue_client_implementation_stages.md)
- [Vue Visual Tuning: Source Guide](./vue_ui_tuning.md)
- [Design, Implementation, And Documentation Workflow](./development_workflow.md)
- [Project Graph Application Architecture](./project_graph_application_architecture.md)
- [Project Undo And Redo Direction](./project_undo_redo_and_backups_direction.md)
- [Package Pipeline Application Architecture](./package_pipeline_architecture.md)
- [Node Definitions And Instances Direction](./node_definitions_and_instances_direction.md)
- [Module Interoperability And Graph Macros Direction](./module_interoperability_and_graph_macros_direction.md)
- [VST Hosting And Presentation Direction](./vst_hosting_and_presentation_direction.md)
- [Configuration Expression Language Direction](./configuration_expression_language_direction.md)
- [GraphBuilder Embedding, Hierarchy, And Project Port Matchers](./graph_builder_embedding_and_matchers.md)
- [Scoped GraphBuilder And SubgraphClosure Direction](./scoped_graph_builder_and_subgraph_closure_direction.md)
- [Node Interaction And Presentation Communication Direction](./node_interaction_and_presentation_communication_direction.md)
- [Vue Output Observation And Query Direction](./vue_output_observation_direction.md)
- [Vue Lane, Meter, Port-Control, Cable, And Automation Interaction Direction](./vue_presentation_refinement_direction.md) (normative owner refinement, 2026-11-16)
- [Node Presentation And Manual Controls Direction](./node_presentation_and_manual_controls_direction.md)
- [System Audio Devices Direction](./system_audio_devices_direction.md)
- [Graph JIT Direction](./graph_jit_direction.md)
- [Realtime / Background Execution, Queues, And Capacity](./realtime_background_execution_and_queues.md)
- [Runtime Performance Roadmap](./runtime_performance_roadmap.md)
- [Batched Node Callback Direction](./batched_node_callbacks_direction.md)
- [DSP Execution And Storage Glossary](./dsp_execution_storage_glossary.md)
- [Output Capabilities, Buffer Access, And Background Demand](./output_capabilities_and_background_demand.md) (normative 2026-11-16 supersession)
- [Circular Global Timeline Indexing](./global_indexing.md)
- [Project Playback And Presentation Grids](./project_playback_and_presentation_grids_direction.md) (normative transport, Event delivery, buffered recording, preparation)
- [Circular Global Index Migration Status](./global_index_migration_status.md)
- [Coverage, Random Access, And Background Evaluation](./coverage_and_background_evaluation.md)
- [Cyclic Automation, Event Projections, And Suspendable Coverage Propagation](./automation_and_suspendable_coverage_direction.md)
- [Automation And Background Planning Migration](./automation_and_background_migration_plan.md) (implementation order)
- [Random-Access Port Data, Audio Value Types, And Input Contract Direction](./random_access_port_data_direction.md)
- [Sequential Port Storage And Connection Planning](./sequential_port_storage_planning.md)
- [Event Propagation Tree Constraint](./event_propagation_tree_constraint.md)
- [Fundamental Event Flows](./event_flows/README.md)

Documents whose governing architecture has been deleted or superseded, along with completed migration/test logs, live under [historical/](./historical/README.md). They are retained for archaeology and rationale only and are not normative for new implementation work.
