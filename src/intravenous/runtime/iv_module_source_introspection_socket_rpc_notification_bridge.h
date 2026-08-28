#pragma once
#include <intravenous/bridge.h>
namespace iv {
class IvModuleSourceIntrospection;
class SocketRpcServer;
IV_DECLARE_BRIDGE(iv_module_source_introspection_socket_rpc_notification_bridge, IvModuleSourceIntrospection, SocketRpcServer);
} // namespace iv
