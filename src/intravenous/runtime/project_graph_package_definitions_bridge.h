#pragma once

#include <intravenous/bridge.h>

namespace iv {
class ProjectGraph;
class PackageDefinitions;
IV_DECLARE_BRIDGE(project_graph_package_definitions_bridge, ProjectGraph, PackageDefinitions);
} // namespace iv
