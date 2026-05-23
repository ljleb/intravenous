#pragma once

namespace iv {
class RuntimeProjectIntrospection;

void bind_iv_module_definitions_project_introspection_bridge(
    RuntimeProjectIntrospection &introspection);
void unbind_iv_module_definitions_project_introspection_bridge(
    RuntimeProjectIntrospection const &introspection);
} // namespace iv
