#include <intravenous/app/bridges/socket_rpc_project_persistence_bridge.h>

#include <intravenous/project/persistence/project_persistence.h>
#include <intravenous/project/runtime_project_events.h>
#include <intravenous/rpc/socket_rpc_server.h>

namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_project_persistence_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_socket_rpc_save_project_event,
    &ProjectPersistence::handle_socket_rpc_save_project)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_socket_rpc_get_session_project_state_event,
    &ProjectPersistence::handle_socket_rpc_get_session_project_state)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_persistence_bridge,
    iv_runtime_project_notification_event,
    &SocketRpcServer::handle_project_notification)
} // namespace iv
