#pragma once

namespace iv {
class RuntimeIvModuleInstances;

void bind_socket_rpc_iv_module_instances_bridge(RuntimeIvModuleInstances &iv_module_instances);
void unbind_socket_rpc_iv_module_instances_bridge(RuntimeIvModuleInstances const &iv_module_instances);
} // namespace iv
