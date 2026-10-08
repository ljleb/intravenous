#pragma once

#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/package_definitions.h>
#include <intravenous/runtime/runtime_project_api_types.h>

#include <nlohmann/json.hpp>

#include <vector>

namespace iv {
    using SocketRpcJson = nlohmann::ordered_json;

    SocketRpcJson source_range_json(SourceRange const &range);
    SocketRpcJson live_source_span_json(LiveSourceSpan const &span);
    SocketRpcJson live_source_spans_json(std::vector<LiveSourceSpan> const &spans);

    SocketRpcJson iv_module_instance_json(IvModuleInstanceInfo const &instance);
    SocketRpcJson iv_module_instances_json(std::vector<IvModuleInstanceInfo> const &instances);
    SocketRpcJson iv_package_json(IvPackageInfo const &package);
    SocketRpcJson iv_package_definitions_json(std::vector<IvPackageInfo> const &packages);

    SocketRpcJson virtual_port_json(VirtualPortInfo const &port);
    SocketRpcJson virtual_ports_json(std::vector<VirtualPortInfo> const &ports);
    SocketRpcJson virtual_node_member_json(VirtualNodeMemberInfo const &member);
    SocketRpcJson virtual_node_members_json(std::vector<VirtualNodeMemberInfo> const &members);
    SocketRpcJson virtual_node_json(VirtualNodeInfo const &node);
    SocketRpcJson virtual_nodes_json(std::vector<VirtualNodeInfo> const &nodes);

} // namespace iv
