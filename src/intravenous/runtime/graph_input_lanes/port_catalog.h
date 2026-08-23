#pragma once

#include <intravenous/runtime/graph_input_lanes.h>

namespace iv {

// Describes the ports advertised by an instance or a graph builder.  It owns
// no lane state; keeping this translation separate makes the reconciliation
// layer solely responsible for deciding which lanes exist.
class GraphInputLanesPortCatalog {
public:
    static auto graph_inputs(IvModuleInstance const& instance)
        -> std::vector<GraphInputLanes::DesiredGraphPort>;
    static auto graph_outputs(IvModuleInstance const& instance)
        -> std::vector<GraphInputLanes::DesiredGraphPort>;
    static auto public_inputs(IvModuleInstance const& instance)
        -> std::vector<GraphInputLanes::DesiredPublicGraphPort>;
    static auto public_outputs(IvModuleInstance const& instance)
        -> std::vector<GraphInputLanes::DesiredPublicGraphPort>;
};
} // namespace iv
