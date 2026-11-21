#pragma once
#include <intravenous/bridge.h>
namespace iv {
class SocketRpcServer;
class GraphExecutor;
IV_DECLARE_BRIDGE(socket_rpc_graph_executor_bridge, SocketRpcServer, GraphExecutor);
}
