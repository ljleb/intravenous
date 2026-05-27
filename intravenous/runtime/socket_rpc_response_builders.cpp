#include "runtime/socket_rpc_response_builders.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace iv {
namespace {
using Json = nlohmann::ordered_json;

std::string serialize_json_line(Json const &value) {
    return value.dump() + "\n";
}

std::string jsonrpc_result(int request_id, Json result_json) {
    return serialize_json_line(Json{
        {"jsonrpc", "2.0"},
        {"id", request_id},
        {"result", std::move(result_json)},
    });
}

std::string jsonrpc_error(int request_id, int code, std::string const &message) {
    return serialize_json_line(Json{
        {"jsonrpc", "2.0"},
        {"id", request_id},
        {"error", {{"code", code}, {"message", message}}},
    });
}

[[noreturn]] void throw_unbuilt_response(char const *builder_name) {
    throw std::runtime_error(
        std::string(builder_name) +
        " did not produce a JSON-RPC response before SocketRpcServer::build()");
}

Json source_range_json(SourceRange const &range) {
    return Json{
        {"start", {{"line", range.start.line}, {"column", range.start.column}}},
        {"end", {{"line", range.end.line}, {"column", range.end.column}}},
    };
}

Json live_source_span_json(LiveSourceSpan const &span) {
    return Json{
        {"filePath", span.file_path},
        {"range", source_range_json(span.range)},
    };
}

Json logical_port_json(LogicalPortInfo const &port) {
    return Json{
        {"ordinal", port.ordinal},
        {"name", port.name},
        {"type", port.type},
    };
}

Json logical_ports_json(std::vector<LogicalPortInfo> const &ports) {
    Json json = Json::array();
    for (auto const &port : ports) {
        json.push_back(logical_port_json(port));
    }
    return json;
}

Json logical_node_member_json(LogicalNodeMemberInfo const &member) {
    return Json{
        {"ordinal", member.ordinal},
        {"backingNodeId", member.backing_node_id},
        {"kind", member.kind},
        {"typeIdentity", member.type_identity},
        {"sampleInputs", logical_ports_json(member.sample_inputs)},
        {"sampleOutputs", logical_ports_json(member.sample_outputs)},
        {"eventInputs", logical_ports_json(member.event_inputs)},
        {"eventOutputs", logical_ports_json(member.event_outputs)},
    };
}

Json logical_node_members_json(std::vector<LogicalNodeMemberInfo> const &members) {
    Json json = Json::array();
    for (auto const &member : members) {
        json.push_back(logical_node_member_json(member));
    }
    return json;
}

Json live_source_spans_json(std::vector<LiveSourceSpan> const &spans) {
    Json json = Json::array();
    for (auto const &span : spans) {
        json.push_back(live_source_span_json(span));
    }
    return json;
}

Json logical_node_json(LogicalNodeInfo const &node) {
    return Json{
        {"id", node.id},
        {"kind", node.kind},
        {"sourceIdentity", node.source_identity},
        {"typeIdentity", node.type_identity},
        {"sourceSpans", live_source_spans_json(node.source_spans)},
        {"sampleInputs", logical_ports_json(node.sample_inputs)},
        {"sampleOutputs", logical_ports_json(node.sample_outputs)},
        {"eventInputs", logical_ports_json(node.event_inputs)},
        {"eventOutputs", logical_ports_json(node.event_outputs)},
        {"memberCount", node.member_count},
        {"members", logical_node_members_json(node.members)},
    };
}

Json logical_nodes_json(std::vector<LogicalNodeInfo> const &nodes) {
    Json json = Json::array();
    for (auto const &node : nodes) {
        json.push_back(logical_node_json(node));
    }
    return json;
}

std::string_view port_kind_json(PortKind kind) {
    switch (kind) {
    case PortKind::sample:
        return "sample";
    case PortKind::event:
        return "event";
    }
    return "sample";
}

std::string_view lane_domain_json(LaneDomain domain) {
    switch (domain) {
    case LaneDomain::compiled:
        return "compiled";
    case LaneDomain::realtime:
        return "realtime";
    }
    return "realtime";
}

Json lane_query_result_json(LaneQueryResult const &result) {
    Json json_lanes = Json::array();
    for (auto const &lane : result.lanes) {
        auto const &graph_input_port = lane.graph_input_port;
        Json port{
            {"logicalNodeId", graph_input_port.logical_node_id},
            {"portKind", std::string(port_kind_json(graph_input_port.port_kind))},
            {"portOrdinal", graph_input_port.port_ordinal},
            {"portName", graph_input_port.port_name},
            {"portType", graph_input_port.port_type},
        };
        if (graph_input_port.concrete_member_ordinal.has_value()) {
            port["memberOrdinal"] = *graph_input_port.concrete_member_ordinal;
        }
        json_lanes.push_back({
            {"laneId", lane.lane_id},
            {"domain", std::string(lane_domain_json(lane.domain))},
            {"graphInputPort", std::move(port)},
        });
    }

    Json json_connections = Json::array();
    for (auto const &connection : result.connections) {
        json_connections.push_back({
            {"sourceLaneId", connection.source_lane_id},
            {"targetLaneId", connection.target_lane_id},
            {"portKind", std::string(port_kind_json(connection.port_kind))},
            {"portOrdinal", connection.port_ordinal},
        });
    }

    return Json{
        {"startIndex", result.start_index},
        {"visibleLaneCount", result.visible_lane_count},
        {"totalLaneCount", result.total_lane_count},
        {"lanes", std::move(json_lanes)},
        {"connections", std::move(json_connections)},
    };
}

Json lane_view_result_json(LaneViewResult const &result) {
    Json json = lane_query_result_json(result.lanes);
    json["viewId"] = result.view_id;
    return json;
}
} // namespace

void SocketRpcAckResponseBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcAckResponseBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcAckResponseBuilder::build(int request_id) const {
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    return jsonrpc_result(request_id, Json{{"ok", true}});
}

void SocketRpcGraphQueryResultBuilder::succeed(ProjectQueryResult value) {
    result = std::move(value);
}

void SocketRpcGraphQueryResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcGraphQueryResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcGraphQueryResultBuilder::build(int request_id) const {
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result.has_value()) {
        throw_unbuilt_response("SocketRpcGraphQueryResultBuilder");
    }
    return jsonrpc_result(request_id, Json{{"nodes", logical_nodes_json(result->nodes)}});
}

void SocketRpcRegionQueryResultBuilder::succeed(ProjectRegionQueryResult value) {
    result = std::move(value);
}

void SocketRpcRegionQueryResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcRegionQueryResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcRegionQueryResultBuilder::build(int request_id) const {
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result.has_value()) {
        throw_unbuilt_response("SocketRpcRegionQueryResultBuilder");
    }
    return jsonrpc_result(
        request_id,
        Json{{"sourceSpans", live_source_spans_json(result->source_spans)}});
}

void SocketRpcLogicalNodeResultBuilder::succeed(LogicalNodeInfo value) {
    result = std::move(value);
}

void SocketRpcLogicalNodeResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcLogicalNodeResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcLogicalNodeResultBuilder::build(int request_id) const {
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result.has_value()) {
        throw_unbuilt_response("SocketRpcLogicalNodeResultBuilder");
    }
    return jsonrpc_result(request_id, logical_node_json(*result));
}

void SocketRpcLogicalNodesResultBuilder::succeed(std::vector<LogicalNodeInfo> value) {
    result = std::move(value);
}

void SocketRpcLogicalNodesResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcLogicalNodesResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcLogicalNodesResultBuilder::build(int request_id) const {
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result.has_value()) {
        throw_unbuilt_response("SocketRpcLogicalNodesResultBuilder");
    }
    return jsonrpc_result(request_id, Json{{"nodes", logical_nodes_json(*result)}});
}

void SocketRpcCreateIvModuleInstanceResultBuilder::succeed(std::string created_instance_id) {
    instance_id = std::move(created_instance_id);
}

void SocketRpcCreateIvModuleInstanceResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcCreateIvModuleInstanceResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcCreateIvModuleInstanceResultBuilder::build(int request_id) const {
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!instance_id.has_value()) {
        throw_unbuilt_response("SocketRpcCreateIvModuleInstanceResultBuilder");
    }
    return jsonrpc_result(request_id, Json{{"instanceId", *instance_id}});
}

void SocketRpcLaneViewResultBuilder::succeed(LaneViewResult value) {
    result = std::move(value);
}

void SocketRpcLaneViewResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcLaneViewResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcLaneViewResultBuilder::build(int request_id) const {
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result.has_value()) {
        throw_unbuilt_response("SocketRpcLaneViewResultBuilder");
    }
    return jsonrpc_result(request_id, lane_view_result_json(*result));
}
} // namespace iv
