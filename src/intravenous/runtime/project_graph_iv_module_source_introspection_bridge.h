#pragma once

#include <intravenous/bridge.h>

namespace iv {
class IvModuleSourceIntrospection;
class ProjectGraph;

// ProjectGraph updates source introspection once per root build, with both the
// definitions publication and the instance declarations it holds.
IV_DECLARE_BRIDGE(
    project_graph_iv_module_source_introspection_bridge,
    ProjectGraph,
    IvModuleSourceIntrospection);
} // namespace iv
