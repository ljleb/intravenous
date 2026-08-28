#include <intravenous/runtime/graph_input_lanes_iv_module_instances_execution_bridge.h>

#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/iv_module_instances_execution.h>

namespace iv {
IV_DEFINE_BRIDGE(graph_input_lanes_iv_module_instances_execution_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    graph_input_lanes_iv_module_instances_execution_bridge,
    iv_runtime_graph_input_lanes_runtime_dependencies_changed_event,
    &IvModuleInstancesExecution::handle_graph_input_lanes_runtime_dependencies_changed);
} // namespace iv
