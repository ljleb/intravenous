#pragma once

namespace iv {
class IvModuleInstancesExecution;

void bind_iv_module_instances_iv_module_instances_execution_bridge(
    IvModuleInstancesExecution &execution);
void unbind_iv_module_instances_iv_module_instances_execution_bridge(
    IvModuleInstancesExecution const &execution);
}
