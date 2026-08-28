#include <intravenous/runtime/graph_input_lanes_timeline_bridge.h>

#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/timeline.h>

namespace iv {
IV_DEFINE_BRIDGE(graph_input_lanes_timeline_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    graph_input_lanes_timeline_bridge,
    iv_runtime_graph_input_lanes_timeline_batch_requested_event,
    &Timeline::handle_graph_input_lanes_timeline_batch)
IV_SUBSCRIBE_SINGLETON_BRIDGE(
    graph_input_lanes_timeline_bridge,
    iv_runtime_graph_input_lanes_knob_value_updated_event,
    &Timeline::handle_graph_input_lanes_knob_value_updated)
IV_SUBSCRIBE_SINGLETON_BRIDGE(
    graph_input_lanes_timeline_bridge,
    iv_runtime_graph_input_lanes_sample_block_published_event,
    &GraphInputLanes::handle_sample_block_published)
IV_SUBSCRIBE_SINGLETON_BRIDGE(
    graph_input_lanes_timeline_bridge,
    iv_runtime_graph_input_lanes_event_block_published_event,
    &GraphInputLanes::handle_event_block_published)
IV_SUBSCRIBE_SINGLETON_BRIDGE(
    graph_input_lanes_timeline_bridge,
    iv_runtime_graph_input_lanes_sample_block_requested_event,
    &GraphInputLanes::handle_graph_input_lanes_sample_block_requested)
IV_SUBSCRIBE_SINGLETON_BRIDGE(
    graph_input_lanes_timeline_bridge,
    iv_runtime_graph_input_lanes_event_block_requested_event,
    &GraphInputLanes::handle_graph_input_lanes_event_block_requested)
} // namespace iv
