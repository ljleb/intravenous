#include <intravenous/runtime/iv_module_instances_iv_module_instances_execution_bridge.h>

#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/iv_module_instances_execution.h>

namespace iv {
IV_DEFINE_BRIDGE(iv_module_instances_iv_module_instances_execution_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    iv_module_instances_iv_module_instances_execution_bridge,
    iv_runtime_iv_module_instance_builders_completed_event,
    &IvModuleInstancesExecution::handle_iv_module_instance_builders_completed);
}
