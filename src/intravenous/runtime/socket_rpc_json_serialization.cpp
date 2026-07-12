#include <intravenous/runtime/socket_rpc_json_serialization.h>

#include <intravenous/sample.h>

#include <string_view>

namespace iv {
namespace {
std::string_view logical_port_connectivity_json(LogicalPortConnectivity connectivity)
{
    switch (connectivity) {
    case LogicalPortConnectivity::disconnected:
        return "disconnected";
    case LogicalPortConnectivity::connected:
        return "connected";
    case LogicalPortConnectivity::mixed:
        return "mixed";
    }
    return "disconnected";
}

std::string_view channel_type_json(ChannelTypeId channel_type)
{
    switch (channel_type) {
    case ChannelTypeId::mono:
        return "mono";
    case ChannelTypeId::stereo:
        return "stereo";
    case ChannelTypeId::count:
        return "mono";
    }
    return "mono";
}

std::string_view port_kind_json(PortKind kind)
{
    switch (kind) {
    case PortKind::sample:
        return "sample";
    case PortKind::event:
        return "event";
    }
    return "sample";
}

std::string_view lane_domain_json(LaneDomain domain)
{
    switch (domain) {
    case LaneDomain::compiled:
        return "compiled";
    case LaneDomain::realtime:
        return "realtime";
    }
    return "realtime";
}
} // namespace

SocketRpcJson source_range_json(SourceRange const &range)
{
    return SocketRpcJson{
        {"start", {{"line", range.start.line}, {"column", range.start.column}}},
        {"end", {{"line", range.end.line}, {"column", range.end.column}}},
    };
}

SocketRpcJson live_source_span_json(LiveSourceSpan const &span)
{
    return SocketRpcJson{
        {"filePath", span.file_path},
        {"range", source_range_json(span.range)},
    };
}

SocketRpcJson live_source_spans_json(std::vector<LiveSourceSpan> const &spans)
{
    SocketRpcJson json = SocketRpcJson::array();
    for (auto const &span : spans) {
        json.push_back(live_source_span_json(span));
    }
    return json;
}

SocketRpcJson iv_module_instance_json(IvModuleInstanceInfo const &instance)
{
    auto const &display_name =
        instance.display_name.empty() ? instance.definition_id : instance.display_name;
    SocketRpcJson json{
        {"instanceId", instance.instance_id},
        {"definitionId", instance.definition_id},
        {"displayName", display_name},
        {"moduleRoot", instance.module_root.generic_string()},
        {"realized", instance.realized},
    };
    if (!instance.module_id.empty()) {
        json["moduleId"] = instance.module_id;
    }
    return json;
}

SocketRpcJson iv_module_instances_json(std::vector<IvModuleInstanceInfo> const &instances)
{
    SocketRpcJson json = SocketRpcJson::array();
    for (auto const &instance : instances) {
        json.push_back(iv_module_instance_json(instance));
    }
    return json;
}

SocketRpcJson iv_module_source_json(IvModuleSourceInfo const &source)
{
    return SocketRpcJson{
        {"moduleId", source.module_id},
        {"moduleRoot", source.module_root.generic_string()},
        {"projectLocal", source.project_local},
    };
}

SocketRpcJson iv_module_sources_json(std::vector<IvModuleSourceInfo> const &sources)
{
    SocketRpcJson json = SocketRpcJson::array();
    for (auto const &source : sources) {
        json.push_back(iv_module_source_json(source));
    }
    return json;
}

SocketRpcJson logical_port_json(LogicalPortInfo const &port)
{
    SocketRpcJson json = SocketRpcJson::object();
    json["ordinal"] = port.ordinal;
    json["name"] = port.name;
    json["type"] = port.type;
    json["connectivity"] = std::string(logical_port_connectivity_json(port.connectivity));
    json["defaultValue"] = static_cast<Sample::storage>(port.default_value);
    json["currentValue"] = static_cast<Sample::storage>(port.current_value);
    json["hasConcreteOverride"] = port.has_concrete_override;
    json["stateValue"] = port.state_value;
    if (port.sample_channel_type.has_value()) {
        json["sampleChannelType"] = std::string(channel_type_json(*port.sample_channel_type));
    }
    return json;
}

SocketRpcJson logical_ports_json(std::vector<LogicalPortInfo> const &ports)
{
    SocketRpcJson json = SocketRpcJson::array();
    for (auto const &port : ports) {
        json.push_back(logical_port_json(port));
    }
    return json;
}

SocketRpcJson logical_node_member_json(LogicalNodeMemberInfo const &member)
{
    return SocketRpcJson{
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

SocketRpcJson logical_node_members_json(std::vector<LogicalNodeMemberInfo> const &members)
{
    SocketRpcJson json = SocketRpcJson::array();
    for (auto const &member : members) {
        json.push_back(logical_node_member_json(member));
    }
    return json;
}

SocketRpcJson logical_node_json(LogicalNodeInfo const &node)
{
    return SocketRpcJson{
        {"id", node.id},
        {"instanceId", node.instance_id},
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

SocketRpcJson logical_nodes_json(std::vector<LogicalNodeInfo> const &nodes)
{
    SocketRpcJson json = SocketRpcJson::array();
    for (auto const &node : nodes) {
        json.push_back(logical_node_json(node));
    }
    return json;
}

SocketRpcJson lane_metadata_json(LaneMetadata const &metadata)
{
    SocketRpcJson json = SocketRpcJson::object();
    for (auto const &key : metadata.unit_values) {
        json[key] = nullptr;
    }
    for (auto const &[key, value] : metadata.int_values) {
        json[key] = value;
    }
    for (auto const &[key, value] : metadata.float_values) {
        json[key] = value;
    }
    return json;
}

SocketRpcJson lane_query_result_json(LaneQueryResult const &result)
{
    SocketRpcJson json_lanes = SocketRpcJson::array();
    for (auto const &lane : result.lanes) {
        json_lanes.push_back(SocketRpcJson{
            {"laneId", lane.lane_id.str()},
            {"domain", std::string(lane_domain_json(lane.domain))},
            {"metadata", lane_metadata_json(lane.metadata)},
        });
    }

    SocketRpcJson json_connections = SocketRpcJson::array();
    for (auto const &connection : result.connections) {
        json_connections.push_back(SocketRpcJson{
            {"sourceLaneId", connection.source_lane_id.str()},
            {"targetLaneId", connection.target_lane_id.str()},
            {"portKind", std::string(port_kind_json(connection.port_kind))},
            {"portOrdinal", connection.port_ordinal},
        });
    }

    SocketRpcJson json = SocketRpcJson{
        {"startIndex", result.start_index},
        {"visibleLaneCount", result.visible_lane_count},
        {"totalLaneCount", result.total_lane_count},
        {"lanes", std::move(json_lanes)},
        {"connections", std::move(json_connections)},
    };
    if (result.error_message.has_value()) {
        json["error"] = *result.error_message;
    }
    return json;
}

SocketRpcJson lane_view_result_json(LaneViewResult const &result)
{
    SocketRpcJson json = lane_query_result_json(result.lanes);
    json["viewId"] = result.view_id.str();
    return json;
}
} // namespace iv
