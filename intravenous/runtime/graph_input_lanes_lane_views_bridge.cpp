#include "runtime/graph_input_lanes_lane_views_bridge.h"

#include "runtime/graph_input_lanes.h"
#include "runtime/lane_views_events.h"

namespace iv {
namespace {
    RuntimeGraphInputLanes *bound_graph_input_lanes = nullptr;

    void handle_query_requested(
        RuntimeLaneViewsQueryRequest const &request,
        RuntimeLaneViewsQueryResultBuilder &builder)
    {
        if (bound_graph_input_lanes == nullptr) {
            return;
        }
        builder.succeed(bound_graph_input_lanes->query_lanes(
            request.filter,
            request.start_index,
            request.visible_lane_count));
    }

    IV_SUBSCRIBE_LINKER_EVENT(
        RuntimeLaneViewsQueryRequestedEvent,
        iv_runtime_lane_views_query_requested_event,
        handle_query_requested);
}

void bind_graph_input_lanes_lane_views_bridge(RuntimeGraphInputLanes &graph_input_lanes)
{
    bound_graph_input_lanes = &graph_input_lanes;
}

void unbind_graph_input_lanes_lane_views_bridge(
    RuntimeGraphInputLanes const &graph_input_lanes)
{
    if (bound_graph_input_lanes == &graph_input_lanes) {
        bound_graph_input_lanes = nullptr;
    }
}
} // namespace iv
