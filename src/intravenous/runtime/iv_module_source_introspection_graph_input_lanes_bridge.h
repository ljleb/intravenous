#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleSourceIntrospection;
class GraphInputLanes;

IV_DECLARE_BRIDGE(
    iv_module_source_introspection_graph_input_lanes_bridge,
    IvModuleSourceIntrospection,
    GraphInputLanes);
} // namespace iv
