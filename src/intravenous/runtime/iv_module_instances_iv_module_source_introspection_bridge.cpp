#include <intravenous/runtime/iv_module_instances_iv_module_source_introspection_bridge.h>

#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>

namespace iv {
IV_DEFINE_BRIDGE(iv_module_instances_iv_module_source_introspection_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    iv_module_instances_iv_module_source_introspection_bridge,
    iv_runtime_iv_module_instances_list_changed_event,
    &IvModuleSourceIntrospection::handle_iv_module_instances_list_changed)
IV_SUBSCRIBE_LINKER_EVENT(
    iv_module_instances_iv_module_source_introspection_bridge,
    iv_runtime_iv_module_instance_builders_completed_event,
    &IvModuleSourceIntrospection::handle_iv_module_instance_builders_completed)
IV_SUBSCRIBE_LINKER_EVENT(
    iv_module_instances_iv_module_source_introspection_bridge,
    iv_runtime_iv_module_instances_source_file_filter_event,
    &IvModuleSourceIntrospection::handle_iv_module_instances_source_file_filter)
} // namespace iv
