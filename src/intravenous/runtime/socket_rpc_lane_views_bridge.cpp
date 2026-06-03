#include <intravenous/runtime/socket_rpc_lane_views_bridge.h>

#include <intravenous/runtime/lane_views.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
namespace {
    LaneViews *bound_lane_views = nullptr;

    void handle_open_lane_view(
        LaneViewRequest const &request,
        SocketRpcLaneViewResultBuilder &builder)
    {
        if (bound_lane_views == nullptr) {
            return;
        }
        builder.succeed(bound_lane_views->open_view(request));
    }

    void handle_update_lane_view(
        LaneViewRequest const &request,
        SocketRpcLaneViewResultBuilder &builder)
    {
        if (bound_lane_views == nullptr) {
            return;
        }
        builder.succeed(bound_lane_views->update_view(request));
    }

    void handle_close_lane_view(
        std::string const &view_id,
        SocketRpcAckResponseBuilder &)
    {
        if (bound_lane_views == nullptr) {
            return;
        }
        bound_lane_views->close_view(view_id);
    }

    IV_SUBSCRIBE_LINKER_EVENT(
        SocketRpcOpenLaneViewEvent,
        iv_socket_rpc_open_lane_view_event,
        handle_open_lane_view);
    IV_SUBSCRIBE_LINKER_EVENT(
        SocketRpcUpdateLaneViewEvent,
        iv_socket_rpc_update_lane_view_event,
        handle_update_lane_view);
    IV_SUBSCRIBE_LINKER_EVENT(
        SocketRpcCloseLaneViewEvent,
        iv_socket_rpc_close_lane_view_event,
        handle_close_lane_view);
} // namespace

void bind_socket_rpc_lane_views_bridge(LaneViews &lane_views)
{
    bound_lane_views = &lane_views;
}

void unbind_socket_rpc_lane_views_bridge(LaneViews const &lane_views)
{
    if (bound_lane_views == &lane_views) {
        bound_lane_views = nullptr;
    }
}
} // namespace iv
