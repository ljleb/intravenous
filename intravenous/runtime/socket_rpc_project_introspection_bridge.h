#pragma once

#include <functional>

namespace iv {
class GraphInputLanes;
class ProjectIntrospection;

void bind_socket_rpc_project_introspection_bridge(
    ProjectIntrospection &introspection,
    GraphInputLanes &graph_input_lanes,
    std::function<void()> *shutdown_callback = nullptr);
void unbind_socket_rpc_project_introspection_bridge(
    ProjectIntrospection const &introspection,
    GraphInputLanes const &graph_input_lanes);
} // namespace iv
