#pragma once

#include <intravenous/bridge.h>

namespace iv {
class GraphInputLanes;
class IvModuleInstancesExecution;

IV_DECLARE_BRIDGE(
    graph_input_lanes_iv_module_instances_execution_bridge,
    GraphInputLanes,
    IvModuleInstancesExecution);
} // namespace iv
