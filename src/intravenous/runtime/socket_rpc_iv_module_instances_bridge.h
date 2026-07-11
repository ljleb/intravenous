#pragma once

namespace iv {
class IvModuleInstances;
class IvModuleSourceIntrospection;
class IvModuleSources;

void bind_socket_rpc_iv_module_instances_bridge(
    IvModuleInstances &iv_module_instances,
    IvModuleSourceIntrospection &introspection,
    IvModuleSources &sources);
void unbind_socket_rpc_iv_module_instances_bridge(
    IvModuleInstances const &iv_module_instances,
    IvModuleSourceIntrospection const &introspection,
    IvModuleSources const &sources);
} // namespace iv
