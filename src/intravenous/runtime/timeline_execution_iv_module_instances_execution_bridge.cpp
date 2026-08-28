#include <intravenous/runtime/timeline_execution_iv_module_instances_execution_bridge.h>

#include <intravenous/runtime/iv_module_instances_execution.h>
#include <intravenous/runtime/timeline_execution_events.h>

namespace iv {
IV_DEFINE_BRIDGE(timeline_execution_iv_module_instances_execution_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    timeline_execution_iv_module_instances_execution_bridge,
    iv_runtime_pause_event,
    &IvModuleInstancesExecution::handle_pause);
IV_SUBSCRIBE_LINKER_EVENT(
    timeline_execution_iv_module_instances_execution_bridge,
    iv_runtime_resume_event,
    &IvModuleInstancesExecution::handle_resume);
IV_SUBSCRIBE_LINKER_EVENT(
    timeline_execution_iv_module_instances_execution_bridge,
    iv_runtime_timeline_execution_resumed_event,
    &IvModuleInstancesExecution::handle_timeline_execution_resumed);
} // namespace iv
