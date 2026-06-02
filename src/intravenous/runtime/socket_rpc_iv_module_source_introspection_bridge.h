#pragma once

#include <functional>

namespace iv {
class GraphInputLanes;
class IvModuleSourceIntrospection;

void bind_socket_rpc_iv_module_source_introspection_bridge(
    IvModuleSourceIntrospection &introspection,
    GraphInputLanes &graph_input_lanes,
    std::function<void()> *shutdown_callback = nullptr);
void unbind_socket_rpc_iv_module_source_introspection_bridge(
    IvModuleSourceIntrospection const &introspection,
    GraphInputLanes const &graph_input_lanes);
} // namespace iv
