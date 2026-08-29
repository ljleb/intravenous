#include <intravenous/runtime/socket_rpc_lane_views_bridge.h>

#include <intravenous/runtime/lane_views.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <intravenous/runtime/lane_views_events.h>
namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_lane_views_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_lane_views_bridge,
    iv_socket_rpc_open_lane_view_event,
    &LaneViews::handle_socket_rpc_open_lane_view)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_lane_views_bridge,
    iv_socket_rpc_update_lane_view_event,
    &LaneViews::handle_socket_rpc_update_lane_view)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_lane_views_bridge,
    iv_socket_rpc_close_lane_view_event,
    &LaneViews::handle_socket_rpc_close_lane_view)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_lane_views_bridge,
    iv_runtime_lane_views_updated_event,
    &SocketRpcServer::handle_lane_views_updated)
} // namespace iv
