#pragma once

#include <functional>

namespace iv {
class RuntimeGraphInputLanes;
class RuntimeProjectIntrospection;

void bind_socket_rpc_project_introspection_bridge(
    RuntimeProjectIntrospection &introspection,
    RuntimeGraphInputLanes &graph_input_lanes,
    std::function<void()> *shutdown_callback = nullptr);
void unbind_socket_rpc_project_introspection_bridge(
    RuntimeProjectIntrospection const &introspection,
    RuntimeGraphInputLanes const &graph_input_lanes);
} // namespace iv
