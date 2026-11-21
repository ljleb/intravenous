#include <intravenous/app/bridges/socket_rpc_graph_executor_bridge.h>
#include <intravenous/rpc/socket_rpc_server.h>
#include <intravenous/execution/graph_executor.h>
namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_graph_executor_bridge)
IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_graph_executor_bridge,
    iv_socket_rpc_playback_event, &GraphExecutor::handle_playback)
}
