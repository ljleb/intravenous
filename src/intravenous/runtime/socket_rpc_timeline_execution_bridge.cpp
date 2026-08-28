#include <intravenous/runtime/socket_rpc_timeline_execution_bridge.h>

#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/timeline_execution.h>

namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_timeline_execution_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_timeline_execution_bridge,
    iv_socket_rpc_pause_event,
    &TimelineExecution::handle_socket_rpc_pause)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_timeline_execution_bridge,
    iv_socket_rpc_resume_event,
    &TimelineExecution::handle_socket_rpc_resume)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_timeline_execution_bridge,
    iv_socket_rpc_seek_event,
    &TimelineExecution::handle_socket_rpc_seek)
} // namespace iv
