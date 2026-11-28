#pragma once

#include <intravenous/project/node_instances.h>
#include <intravenous/packages/package_definitions.h>
#include <intravenous/project/runtime_project_api_types.h>

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

    SocketRpcJson symbolic_continuous_port_json(
        SymbolicContinuousPortInfo const& port);
    SocketRpcJson continuous_value_types_json(
        std::vector<ContinuousValueIntrospectionMetadata> const& metadata);
    SocketRpcJson symbolic_continuous_ports_json(
        std::vector<SymbolicContinuousPortInfo> const& ports);
    SocketRpcJson symbolic_event_port_json(SymbolicEventPortInfo const& port);
    SocketRpcJson symbolic_event_ports_json(
        std::vector<SymbolicEventPortInfo> const& ports);
    SocketRpcJson symbolic_node_member_json(SymbolicNodeMemberInfo const &member);
    SocketRpcJson symbolic_node_members_json(std::vector<SymbolicNodeMemberInfo> const &members);
    SocketRpcJson symbolic_node_json(SymbolicNodeInfo const &node);
    SocketRpcJson symbolic_nodes_json(std::vector<SymbolicNodeInfo> const &nodes);

} // namespace iv
