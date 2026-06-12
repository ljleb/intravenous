#pragma once

namespace iv {
class IvModuleInstancesExecution;

void bind_graph_input_lanes_iv_module_instances_execution_bridge(
    IvModuleInstancesExecution &execution);
void unbind_graph_input_lanes_iv_module_instances_execution_bridge(
    IvModuleInstancesExecution const &execution);
}
