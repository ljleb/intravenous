#pragma once
#include <intravenous/bridge.h>
namespace iv {
class Observations;
class ProjectGraph;
class GraphExecutor;
class SocketRpcServer;
IV_DECLARE_BRIDGE(observations_project_graph_bridge, Observations, ProjectGraph);
IV_DECLARE_BRIDGE(observations_graph_executor_bridge, Observations, GraphExecutor);
IV_DECLARE_BRIDGE(observations_socket_rpc_bridge, Observations, SocketRpcServer);
IV_DECLARE_BRIDGE(socket_rpc_observations_bridge, SocketRpcServer, Observations);
}
