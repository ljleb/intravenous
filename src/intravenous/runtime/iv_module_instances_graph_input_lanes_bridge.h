#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleInstances;
class GraphInputLanes;

IV_DECLARE_BRIDGE(
    iv_module_instances_graph_input_lanes_bridge,
    IvModuleInstances,
    GraphInputLanes);
} // namespace iv
