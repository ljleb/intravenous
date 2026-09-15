#include <intravenous/runtime/iv_module_source_introspection_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    IvModuleSourceIntrospectionNodesUpdatedEvent,
    iv_runtime_iv_module_source_introspection_nodes_updated_event)
IV_DEFINE_LINKER_EVENT(
    IvModuleInstancesSourceFileFilterEvent,
    iv_runtime_iv_module_instances_source_file_filter_event)
} // namespace iv
