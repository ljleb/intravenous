#pragma once
#include <intravenous/bridge.h>
namespace iv {
class IvModuleInstances;
class SocketRpcServer;
IV_DECLARE_BRIDGE(iv_module_instances_socket_rpc_notification_bridge, IvModuleInstances, SocketRpcServer);
} // namespace iv
