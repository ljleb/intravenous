#include <intravenous/runtime/lanes_visualization_socket_rpc_notification_bridge.h>
#include <intravenous/runtime/lanes_visualization_events.h>
#include <intravenous/runtime/socket_rpc_server.h>
namespace iv {
IV_DEFINE_BRIDGE(lanes_visualization_socket_rpc_notification_bridge)
IV_SUBSCRIBE_LINKER_EVENT(lanes_visualization_socket_rpc_notification_bridge, iv_runtime_lane_view_content_updated_event, &SocketRpcServer::handle_lane_view_content_updated)
} // namespace iv
