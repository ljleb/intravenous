#include <intravenous/runtime/lane_views_socket_rpc_notification_bridge.h>
#include <intravenous/runtime/lane_views_events.h>
#include <intravenous/runtime/socket_rpc_server.h>
namespace iv {
IV_DEFINE_BRIDGE(lane_views_socket_rpc_notification_bridge)
IV_SUBSCRIBE_LINKER_EVENT(lane_views_socket_rpc_notification_bridge, iv_runtime_lane_views_updated_event, &SocketRpcServer::handle_lane_views_updated)
} // namespace iv
