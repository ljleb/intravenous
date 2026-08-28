#include <intravenous/runtime/project_persistence_iv_module_instances_bridge.h>

#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
IV_DEFINE_BRIDGE(project_persistence_iv_module_instances_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_iv_module_instances_bridge,
    iv_runtime_project_create_iv_module_instance_requested_event,
    &IvModuleInstances::handle_project_create_iv_module_instance)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_iv_module_instances_bridge,
    iv_runtime_project_delete_iv_module_instance_requested_event,
    &IvModuleInstances::handle_project_delete_iv_module_instance)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_iv_module_instances_bridge,
    iv_runtime_project_update_iv_module_instances_requested_event,
    &IvModuleInstances::handle_project_update_iv_module_instances)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_iv_module_instances_bridge,
    iv_runtime_project_persistence_collect_state_event,
    &IvModuleInstances::handle_project_persistence_collect_state)
} // namespace iv
