#include <intravenous/runtime/iv_module_instances_graph_input_lanes_bridge.h>

#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/iv_module_instances_events.h>

namespace iv {
IV_DEFINE_BRIDGE(iv_module_instances_graph_input_lanes_bridge)
IV_SUBSCRIBE_BRIDGE(
    iv_module_instances_graph_input_lanes_bridge,
    iv_runtime_iv_module_instance_builders_changed_event,
    static_cast<void (GraphInputLanes::*)(
        IvModuleInstanceBuildersChanged const &,
        IvModuleInstanceBuildersAckBuilder &)>(
        &GraphInputLanes::handle_iv_module_instance_builders_changed));
} // namespace iv
