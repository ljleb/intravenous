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

std::string_view lane_query_value_type_json(query::LaneQueryValueType type)
{
    switch (type) {
    case query::LaneQueryValueType::unit:
        return "unit";
    case query::LaneQueryValueType::int_:
        return "int";
    case query::LaneQueryValueType::float_:
        return "float";
    }
    return "unit";
}

SocketRpcJson lane_query_schema_entry_json(query::LaneQuerySchemaEntry const &entry)
{
    return SocketRpcJson{
        {"key", entry.key},
        {"type", std::string(lane_query_value_type_json(entry.type))},
    };
}

std::string_view lane_query_completion_context_json(query::LaneQueryCompletionContext context)
{
    switch (context) {
    case query::LaneQueryCompletionContext::property:
        return "property";
    case query::LaneQueryCompletionContext::value:
        return "value";
    case query::LaneQueryCompletionContext::projection_selector:
        return "projectionSelector";
    case query::LaneQueryCompletionContext::syntax:
        return "syntax";
    }
    return "property";
}

std::string_view lane_query_completion_kind_json(query::LaneQueryCompletionKind kind)
{
    switch (kind) {
    case query::LaneQueryCompletionKind::property:
        return "property";
    case query::LaneQueryCompletionKind::unit_literal:
        return "unitLiteral";
    case query::LaneQueryCompletionKind::numeric_literal:
        return "numericLiteral";
    case query::LaneQueryCompletionKind::numeric_range:
        return "numericRange";
    case query::LaneQueryCompletionKind::syntax:
        return "syntax";
    }
    return "property";
}

SocketRpcJson lane_query_source_range_json(query::LaneQuerySourceRange const &range)
{
    return SocketRpcJson{
        {"startOffset", range.start_offset},
        {"endOffset", range.end_offset},
    };
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
    json["minValue"] = port.min.has_value()
        ? SocketRpcJson(static_cast<Sample::storage>(*port.min))
        : SocketRpcJson(nullptr);
    json["maxValue"] = port.max.has_value()
        ? SocketRpcJson(static_cast<Sample::storage>(*port.max))
        : SocketRpcJson(nullptr);
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
    for (auto const &[key, value] : metadata.string_values) {
        json[key] = value;
    }
    return json;
}

SocketRpcJson lane_query_result_json(LaneQueryResult const &result)
{
    SocketRpcJson json_lanes = SocketRpcJson::array();
    for (auto const &lane : result.lanes) {
        auto json_lane = SocketRpcJson{
            {"laneId", lane.lane_id.str()},
            {"domain", std::string(lane_domain_json(lane.domain))},
            {"metadata", lane_metadata_json(lane.metadata)},
        };
        if (lane.model_type_id.has_value()) {
            json_lane["modelTypeId"] = *lane.model_type_id;
        }
        if (lane.sample_channel_type.has_value()) {
            json_lane["sampleChannelType"] = std::string(channel_type_json(*lane.sample_channel_type));
        }
        json_lane["outputKind"] = std::string(port_kind_json(lane.output_kind));
        auto json_inputs = SocketRpcJson::array();
        for (auto const &input : lane.inputs) {
            json_inputs.push_back(SocketRpcJson{
                {"domain", input.domain == LanePortDomain::compiled ? "compiled" : "realtime"},
                {"kind", std::string(port_kind_json(input.kind))},
                {"ordinal", input.ordinal},
                {"name", input.name},
            });
        }
        json_lane["inputs"] = std::move(json_inputs);
        json_lanes.push_back(std::move(json_lane));
    }

    SocketRpcJson json_connections = SocketRpcJson::array();
    for (auto const &connection : result.connections) {
        json_connections.push_back(SocketRpcJson{
            {"sourceLaneId", connection.source_lane_id.str()},
            {"targetLaneId", connection.target_lane_id.str()},
            {"portDomain", connection.port_domain == LanePortDomain::compiled ? "compiled" : "realtime"},
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

SocketRpcJson lane_query_schema_json(query::LaneQuerySchema const &schema)
{
    SocketRpcJson entries = SocketRpcJson::array();
    for (auto const &entry : schema.entries()) {
        entries.push_back(lane_query_schema_entry_json(entry));
    }
    return SocketRpcJson{
        {"revision", schema.revision()},
        {"entries", std::move(entries)},
    };
}

SocketRpcJson lane_query_schema_change_json(query::LaneQuerySchemaChange const &change)
{
    SocketRpcJson added = SocketRpcJson::array();
    for (auto const &item : change.added) {
        added.push_back(lane_query_schema_entry_json(item.entry));
    }
    SocketRpcJson removed = SocketRpcJson::array();
    for (auto const &item : change.removed) {
        removed.push_back(lane_query_schema_entry_json(item.entry));
    }
    SocketRpcJson retyped = SocketRpcJson::array();
    for (auto const &item : change.retyped) {
        retyped.push_back(SocketRpcJson{
            {"key", item.key},
            {"oldType", std::string(lane_query_value_type_json(item.old_type))},
            {"newType", std::string(lane_query_value_type_json(item.new_type))},
        });
    }
    return SocketRpcJson{
        {"oldRevision", change.old_revision},
        {"revision", change.new_revision},
        {"added", std::move(added)},
        {"removed", std::move(removed)},
        {"retyped", std::move(retyped)},
    };
}

SocketRpcJson lane_query_completion_json(
    query::LaneQueryCompletionResult const &result,
    std::uint64_t schema_revision)
{
    SocketRpcJson candidates = SocketRpcJson::array();
    for (auto const &candidate : result.candidates) {
        SocketRpcJson json{
            {"kind", std::string(lane_query_completion_kind_json(candidate.kind))},
            {"label", candidate.label},
            {"insertText", candidate.insert_text},
        };
        if (candidate.value_type.has_value()) {
            json["valueType"] = std::string(lane_query_value_type_json(*candidate.value_type));
        }
        candidates.push_back(std::move(json));
    }
    SocketRpcJson diagnostics = SocketRpcJson::array();
    for (auto const &diagnostic : result.diagnostics) {
        diagnostics.push_back(SocketRpcJson{
            {"range", lane_query_source_range_json(diagnostic.range)},
            {"message", diagnostic.message},
        });
    }
    return SocketRpcJson{
        {"schemaRevision", schema_revision},
        {"replacementRange", lane_query_source_range_json(result.replacement_range)},
        {"context", std::string(lane_query_completion_context_json(result.context))},
        {"candidates", std::move(candidates)},
        {"diagnostics", std::move(diagnostics)},
    };
}
} // namespace iv
