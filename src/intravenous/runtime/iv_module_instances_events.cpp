#include <intravenous/runtime/iv_module_instances_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    IvModuleRequiredDefinitionsChangedEvent,
    iv_runtime_iv_module_required_definitions_changed_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleInstancesChangedEvent,
    iv_runtime_iv_module_instances_changed_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleInstanceBuildersChangedEvent,
    iv_runtime_iv_module_instance_builders_changed_event);
IV_DEFINE_LINKER_EVENT(
    IvModuleInstancesListChangedEvent,
    iv_runtime_iv_module_instances_list_changed_event);
} // namespace iv
