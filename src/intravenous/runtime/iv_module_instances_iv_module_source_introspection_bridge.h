#pragma once

namespace iv {
class IvModuleSourceIntrospection;

void bind_iv_module_instances_iv_module_source_introspection_bridge(
    IvModuleSourceIntrospection &introspection);
void unbind_iv_module_instances_iv_module_source_introspection_bridge(
    IvModuleSourceIntrospection const &introspection);
} // namespace iv
