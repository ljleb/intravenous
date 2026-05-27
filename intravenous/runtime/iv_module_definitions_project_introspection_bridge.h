#pragma once

namespace iv {
class ProjectIntrospection;

void bind_iv_module_definitions_project_introspection_bridge(
    ProjectIntrospection &introspection);
void unbind_iv_module_definitions_project_introspection_bridge(
    ProjectIntrospection const &introspection);
} // namespace iv
