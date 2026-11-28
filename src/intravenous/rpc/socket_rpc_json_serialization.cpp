#include <intravenous/rpc/socket_rpc_json_serialization.h>

#include <intravenous/graph/names.h>

#include <string>
#include <string_view>
#include <utility>

namespace iv {
namespace {
std::string_view symbolic_port_connectivity_json(SymbolicPortConnectivity connectivity)
{
    switch (connectivity) {
    case SymbolicPortConnectivity::disconnected:
        return "disconnected";
    case SymbolicPortConnectivity::connected:
        return "connected";
    case SymbolicPortConnectivity::mixed:
        return "mixed";
    }
    return "disconnected";
}

std::string_view channel_type_json(ChannelTypeId channel_type)
{
    return channel_type_info(channel_type).wire_name;
}

std::string_view symbolic_port_binding_json(SymbolicPortBinding binding)
{
    switch (binding) {
    case SymbolicPortBinding::interface:
        return "interface";
    case SymbolicPortBinding::none:
        return "none";
    case SymbolicPortBinding::symbolic_input:
        return "symbolicInput";
    case SymbolicPortBinding::symbolic_output:
        return "symbolicOutput";
    case SymbolicPortBinding::mixed:
        return "mixed";
    }
    return "none";
}

std::string_view continuous_channel_capability_json(
    ContinuousValueChannelCapability capability)
{
    switch (capability) {
    case ContinuousValueChannelCapability::scalar:
        return "scalar";
    case ContinuousValueChannelCapability::audio:
        return "audio";
    }
    return "scalar";
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

std::string_view package_provenance_json(PackageProvenance provenance)
{
    switch (provenance) {
    case PackageProvenance::built_in:
        return "builtIn";
    case PackageProvenance::common:
        return "common";
    case PackageProvenance::project_local:
        return "projectLocal";
    }
    return "projectLocal";
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
        {"defaultDisplayName", instance.default_display_name.empty() ? instance.definition_id : instance.default_display_name},
        {"configurationSource", instance.configuration_source},
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
        {"provenance", package_provenance_json(package.provenance)},
        {"packageNamespace", package.package_namespace},
        {"packageDependencies", package.package_dependencies},
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

namespace {

SocketRpcJson continuous_value_type_json(
    ContinuousValueIntrospectionMetadata const& metadata)
{
    auto const type = continuous_introspection_value_type(metadata);
    auto const& info = continuous_value_type_info(type);
    SocketRpcJson json = SocketRpcJson::object();
    json["type"] = std::string(info.name);
    json["rank"] = info.rank;
    json["channelCapability"] = std::string(
        continuous_channel_capability_json(info.channel_capability));
    std::visit([&](auto const& type_metadata) {
        type_metadata.write_json(json);
    }, metadata);
    return json;
}

} // namespace

SocketRpcJson continuous_value_types_json(
    std::vector<ContinuousValueIntrospectionMetadata> const& metadata)
{
    SocketRpcJson json = SocketRpcJson::array();
    for (auto const& value : metadata) {
        json.push_back(continuous_value_type_json(value));
    }
    return json;
}

SocketRpcJson symbolic_continuous_port_json(
    SymbolicContinuousPortInfo const& port)
{
    SocketRpcJson json = SocketRpcJson::object();
    json["index"] = port.index;
    json["name"] = port.name;
    if (!port.display_name.empty()) json["displayName"] = port.display_name;
    if (!port.short_display_name.empty()) json["shortDisplayName"] = port.short_display_name;
    json["connectivity"] = std::string(symbolic_port_connectivity_json(port.connectivity));
    json["binding"] = std::string(symbolic_port_binding_json(port.binding));
    json["valueTypes"] = continuous_value_types_json(port.value_types);
    if (port.channel_type.has_value()) {
        json["channelType"] = std::string(channel_type_json(*port.channel_type));
    }
    return json;
}

SocketRpcJson symbolic_continuous_ports_json(
    std::vector<SymbolicContinuousPortInfo> const& ports)
{
    SocketRpcJson json = SocketRpcJson::array();
    for (auto const &port : ports) {
        json.push_back(symbolic_continuous_port_json(port));
    }
    return json;
}

SocketRpcJson symbolic_event_port_json(SymbolicEventPortInfo const& port)
{
    SocketRpcJson json{
        {"index", port.index},
        {"name", port.name},
        {"connectivity", std::string(
            symbolic_port_connectivity_json(port.connectivity))},
        {"binding", std::string(symbolic_port_binding_json(port.binding))},
        {"eventType", std::string(details::event_type_name(port.type))},
    };
    if (!port.display_name.empty()) json["displayName"] = port.display_name;
    if (!port.short_display_name.empty()) json["shortDisplayName"] = port.short_display_name;
    return json;
}

SocketRpcJson symbolic_event_ports_json(
    std::vector<SymbolicEventPortInfo> const& ports)
{
    SocketRpcJson json = SocketRpcJson::array();
    for (auto const& port : ports) {
        json.push_back(symbolic_event_port_json(port));
    }
    return json;
}

SocketRpcJson symbolic_node_member_json(SymbolicNodeMemberInfo const &member)
{
    return SocketRpcJson{
        {"index", member.index},
        {"backingNodeId", member.backing_node_id},
        {"kind", member.kind},
        {"typeIdentity", member.type_identity},
        {"definitionId", member.definition_id},
        {"continuousInputs", symbolic_continuous_ports_json(
            member.continuous_inputs)},
        {"continuousOutputs", symbolic_continuous_ports_json(
            member.continuous_outputs)},
        {"eventInputs", symbolic_event_ports_json(member.event_inputs)},
        {"eventOutputs", symbolic_event_ports_json(member.event_outputs)},
    };
}

SocketRpcJson symbolic_node_members_json(std::vector<SymbolicNodeMemberInfo> const &members)
{
    SocketRpcJson json = SocketRpcJson::array();
    for (auto const &member : members) {
        json.push_back(symbolic_node_member_json(member));
    }
    return json;
}

SocketRpcJson symbolic_node_json(SymbolicNodeInfo const &node)
{
    return SocketRpcJson{
        {"id", node.id},
        {"instanceId", node.instance_id},
        {"kind", node.kind},
        {"sourceIdentity", node.source_identity},
        {"typeIdentity", node.type_identity},
        {"definitionId", node.definition_id},
        {"sourceSpans", live_source_spans_json(node.source_spans)},
        {"continuousInputs", symbolic_continuous_ports_json(
            node.continuous_inputs)},
        {"continuousOutputs", symbolic_continuous_ports_json(
            node.continuous_outputs)},
        {"eventInputs", symbolic_event_ports_json(node.event_inputs)},
        {"eventOutputs", symbolic_event_ports_json(node.event_outputs)},
        {"memberCount", node.member_count},
        {"members", symbolic_node_members_json(node.members)},
    };
}

SocketRpcJson symbolic_nodes_json(std::vector<SymbolicNodeInfo> const &nodes)
{
    SocketRpcJson json = SocketRpcJson::array();
    for (auto const &node : nodes) {
        json.push_back(symbolic_node_json(node));
    }
    return json;
}

} // namespace iv
