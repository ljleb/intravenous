#include <intravenous/runtime/socket_rpc_project_autosave_bridge.h>

#include <intravenous/runtime/project_autosave.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_project_autosave_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_autosave_bridge,
    iv_socket_rpc_enable_project_autosave_event,
    &ProjectAutosave::handle_socket_rpc_enable_project_autosave);
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_project_autosave_bridge,
    iv_socket_rpc_disable_project_autosave_event,
    &ProjectAutosave::handle_socket_rpc_disable_project_autosave);
} // namespace iv
