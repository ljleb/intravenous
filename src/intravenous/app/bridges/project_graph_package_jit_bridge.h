#pragma once
#include <intravenous/bridge.h>
namespace iv {
class ProjectGraph;
class PackageJit;
IV_DECLARE_BRIDGE(project_graph_package_jit_bridge, ProjectGraph, PackageJit);
} // namespace iv
