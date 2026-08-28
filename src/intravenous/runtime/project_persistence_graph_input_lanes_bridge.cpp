#include <intravenous/runtime/project_persistence_graph_input_lanes_bridge.h>

#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
IV_DEFINE_BRIDGE(project_persistence_graph_input_lanes_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_graph_input_lanes_bridge,
    iv_runtime_project_set_sample_input_value_requested_event,
    &GraphInputLanes::handle_project_set_sample_input_value)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_graph_input_lanes_bridge,
    iv_runtime_project_set_sample_input_state_requested_event,
    &GraphInputLanes::handle_project_set_sample_input_state)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_graph_input_lanes_bridge,
    iv_runtime_project_set_public_sample_input_state_requested_event,
    &GraphInputLanes::handle_project_set_public_sample_input_state)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_graph_input_lanes_bridge,
    iv_runtime_project_set_public_sample_input_value_requested_event,
    &GraphInputLanes::handle_project_set_public_sample_input_value)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_graph_input_lanes_bridge,
    iv_runtime_project_set_event_input_state_requested_event,
    &GraphInputLanes::handle_project_set_event_input_state)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_graph_input_lanes_bridge,
    iv_runtime_project_set_sample_output_state_requested_event,
    &GraphInputLanes::handle_project_set_sample_output_state)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_graph_input_lanes_bridge,
    iv_runtime_project_set_event_output_state_requested_event,
    &GraphInputLanes::handle_project_set_event_output_state)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_graph_input_lanes_bridge,
    iv_runtime_project_persistence_collect_state_event,
    &GraphInputLanes::handle_project_persistence_collect_state)
} // namespace iv
