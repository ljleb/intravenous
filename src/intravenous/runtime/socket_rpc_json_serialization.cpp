#include <intravenous/runtime/socket_rpc_json_serialization.h>

#include <intravenous/sample.h>

#include <string_view>

namespace iv {
namespace {
std::string_view virtual_port_connectivity_json(VirtualPortConnectivity connectivity)
{
    switch (connectivity) {
    case VirtualPortConnectivity::disconnected:
        return "disconnected";
    case VirtualPortConnectivity::connected:
        return "connected";
    case VirtualPortConnectivity::mixed:
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

std::string_view package_build_state_json(PackageBuildState state)
{
    switch (state) {
    case PackageBuildState::queued:
        return "queued";
    case PackageBuildState::building:
        return "building";
    case PackageBuildState::built:
        return "built";
    case PackageBuildState::failed:
        return "failed";
    }
    return "queued";
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
        {"packageRoot", instance.package_root.generic_string()},
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

SocketRpcJson iv_package_json(IvPackageInfo const &package)
{
    return SocketRpcJson{
        {"packageId", package.package_id},
        {"packageRoot", package.package_root.generic_string()},
        {"projectLocal", package.project_local},
        {"moduleIds", package.module_ids},
        {"nodeTypeIds", package.node_type_ids},
        {"buildState", package_build_state_json(package.build_state)},
        {"buildMessage", package.build_message},
        {"publicationMessage", package.publication_message},
    };
}

SocketRpcJson iv_package_definitions_json(std::vector<IvPackageInfo> const &packages)
{
    SocketRpcJson json = SocketRpcJson::array();
    for (auto const &package : packages) {
        json.push_back(iv_package_json(package));
    }
    return json;
}

SocketRpcJson virtual_port_json(VirtualPortInfo const &port)
{
    SocketRpcJson json = SocketRpcJson::object();
    json["index"] = port.index;
    json["name"] = port.name;
    json["type"] = port.type;
    json["connectivity"] = std::string(virtual_port_connectivity_json(port.connectivity));
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

SocketRpcJson virtual_ports_json(std::vector<VirtualPortInfo> const &ports)
{
    SocketRpcJson json = SocketRpcJson::array();
    for (auto const &port : ports) {
        json.push_back(virtual_port_json(port));
    }
    return json;
}

SocketRpcJson virtual_node_member_json(VirtualNodeMemberInfo const &member)
{
    return SocketRpcJson{
        {"index", member.index},
        {"backingNodeId", member.backing_node_id},
        {"kind", member.kind},
        {"typeIdentity", member.type_identity},
        {"sampleInputs", virtual_ports_json(member.sample_inputs)},
        {"sampleOutputs", virtual_ports_json(member.sample_outputs)},
        {"eventInputs", virtual_ports_json(member.event_inputs)},
        {"eventOutputs", virtual_ports_json(member.event_outputs)},
    };
}

SocketRpcJson virtual_node_members_json(std::vector<VirtualNodeMemberInfo> const &members)
{
    SocketRpcJson json = SocketRpcJson::array();
    for (auto const &member : members) {
        json.push_back(virtual_node_member_json(member));
    }
    return json;
}

SocketRpcJson virtual_node_json(VirtualNodeInfo const &node)
{
    return SocketRpcJson{
        {"id", node.id},
        {"instanceId", node.instance_id},
        {"kind", node.kind},
        {"sourceIdentity", node.source_identity},
        {"typeIdentity", node.type_identity},
        {"sourceSpans", live_source_spans_json(node.source_spans)},
        {"sampleInputs", virtual_ports_json(node.sample_inputs)},
        {"sampleOutputs", virtual_ports_json(node.sample_outputs)},
        {"eventInputs", virtual_ports_json(node.event_inputs)},
        {"eventOutputs", virtual_ports_json(node.event_outputs)},
        {"memberCount", node.member_count},
        {"members", virtual_node_members_json(node.members)},
    };
}

SocketRpcJson virtual_nodes_json(std::vector<VirtualNodeInfo> const &nodes)
{
    SocketRpcJson json = SocketRpcJson::array();
    for (auto const &node : nodes) {
        json.push_back(virtual_node_json(node));
    }
    return json;
}

} // namespace iv
