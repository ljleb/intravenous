#include <intravenous/runtime/socket_rpc_project_persistence_bridge.h>

#include <intravenous/runtime/project_persistence.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_project_persistence_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_socket_rpc_save_project_event,
    &ProjectPersistence::handle_socket_rpc_save_project)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_runtime_project_notification_event,
    &SocketRpcServer::handle_project_notification)
} // namespace iv
