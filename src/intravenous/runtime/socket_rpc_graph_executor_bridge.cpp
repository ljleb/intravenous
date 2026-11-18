#include <intravenous/runtime/socket_rpc_graph_executor_bridge.h>
#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/graph_executor.h>
namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_graph_executor_bridge)
IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_graph_executor_bridge,
    iv_socket_rpc_playback_event, &GraphExecutor::handle_playback)
}
