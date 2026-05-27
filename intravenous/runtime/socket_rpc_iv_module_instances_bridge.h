#pragma once

namespace iv {
class IvModuleInstances;

void bind_socket_rpc_iv_module_instances_bridge(IvModuleInstances &iv_module_instances);
void unbind_socket_rpc_iv_module_instances_bridge(IvModuleInstances const &iv_module_instances);
} // namespace iv
