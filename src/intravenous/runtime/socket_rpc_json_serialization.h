#pragma once

#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_sources.h>
#include <intravenous/runtime/lane_view_service.h>
#include <intravenous/runtime/runtime_project_api_types.h>
#include <intravenous/query/lane_query_schema.h>
#include <intravenous/query/lane_query_completion.h>

#include <nlohmann/json.hpp>

#include <vector>

namespace iv {
    using SocketRpcJson = nlohmann::ordered_json;

    SocketRpcJson source_range_json(SourceRange const &range);
    SocketRpcJson live_source_span_json(LiveSourceSpan const &span);
    SocketRpcJson live_source_spans_json(std::vector<LiveSourceSpan> const &spans);

    SocketRpcJson iv_module_instance_json(IvModuleInstanceInfo const &instance);
    SocketRpcJson iv_module_instances_json(std::vector<IvModuleInstanceInfo> const &instances);
    SocketRpcJson iv_module_source_json(IvModuleSourceInfo const &source);
    SocketRpcJson iv_module_sources_json(std::vector<IvModuleSourceInfo> const &sources);

    SocketRpcJson logical_port_json(LogicalPortInfo const &port);
    SocketRpcJson logical_ports_json(std::vector<LogicalPortInfo> const &ports);
    SocketRpcJson logical_node_member_json(LogicalNodeMemberInfo const &member);
    SocketRpcJson logical_node_members_json(std::vector<LogicalNodeMemberInfo> const &members);
    SocketRpcJson logical_node_json(LogicalNodeInfo const &node);
    SocketRpcJson logical_nodes_json(std::vector<LogicalNodeInfo> const &nodes);

    SocketRpcJson lane_metadata_json(LaneMetadata const &metadata);
    SocketRpcJson lane_query_result_json(LaneQueryResult const &result);
    SocketRpcJson lane_view_result_json(LaneViewResult const &result);
    SocketRpcJson lane_query_schema_json(query::LaneQuerySchema const &schema);
    SocketRpcJson lane_query_schema_change_json(query::LaneQuerySchemaChange const &change);
    SocketRpcJson lane_query_completion_json(
        query::LaneQueryCompletionResult const &result,
        std::uint64_t schema_revision);
} // namespace iv
