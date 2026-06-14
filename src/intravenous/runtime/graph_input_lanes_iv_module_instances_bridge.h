#pragma once

namespace iv {
class IvModuleInstances;

void bind_graph_input_lanes_iv_module_instances_bridge(
    IvModuleInstances &instances);
void unbind_graph_input_lanes_iv_module_instances_bridge(
    IvModuleInstances const &instances);
} // namespace iv
