#pragma once
#include <intravenous/bridge.h>
namespace iv {
class IvModuleDefinitions;
class SocketRpcServer;
IV_DECLARE_BRIDGE(iv_module_definitions_socket_rpc_notification_bridge, IvModuleDefinitions, SocketRpcServer);
} // namespace iv
