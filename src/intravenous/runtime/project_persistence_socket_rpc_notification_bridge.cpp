#include <intravenous/runtime/project_persistence_socket_rpc_notification_bridge.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_server.h>
namespace iv {
IV_DEFINE_BRIDGE(project_persistence_socket_rpc_notification_bridge)
IV_SUBSCRIBE_LINKER_EVENT(project_persistence_socket_rpc_notification_bridge, iv_runtime_project_notification_event, &SocketRpcServer::handle_project_notification)
} // namespace iv
