#include <intravenous/runtime/iv_module_source_introspection_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    IvModuleSourceIntrospectionLiveInputSnapshotsRequestedEvent,
    iv_runtime_iv_module_source_introspection_live_input_snapshots_requested_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleSourceIntrospectionAuthoredStateSnapshotRequestedEvent,
    iv_runtime_iv_module_source_introspection_authored_state_snapshot_requested_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleSourceIntrospectionPublicPortsSnapshotRequestedEvent,
    iv_runtime_iv_module_source_introspection_public_ports_snapshot_requested_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleSourceIntrospectionNodesUpdatedEvent,
    iv_runtime_iv_module_source_introspection_nodes_updated_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleInstancesSourceFileFilterEvent,
    iv_runtime_iv_module_instances_source_file_filter_event);
} // namespace iv
