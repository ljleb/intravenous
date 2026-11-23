#include <intravenous/rpc/socket_rpc_response_builders.h>
#include <intravenous/rpc/socket_rpc_json_serialization.h>
#include <intravenous/project/persistence/project_connection_json.h>
#include <intravenous/project/persistence/project_presentation_json.h>

#include <intravenous/channel_type.h>
#include <intravenous/graph/names.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

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

Json audio_device_descriptor_json(AudioDeviceDescriptor const &device)
{
    return Json{
        {"deviceId", device.device_id},
        {"name", device.name},
    };
}

Json audio_device_descriptors_json(std::vector<AudioDeviceDescriptor> const &devices)
{
    Json json = Json::array();
    for (auto const &device : devices) {
        json.push_back(audio_device_descriptor_json(device));
    }
    return json;
}

Json audio_device_selection_json(AudioDeviceSelectionState const &selection)
{
    Json json = Json{
        {"deviceId", selection.device_id.has_value() ? Json(*selection.device_id) : Json(nullptr)},
        {"name", selection.name.has_value() ? Json(*selection.name) : Json(nullptr)},
        {"available", selection.available},
        {"error", selection.error ? Json(*selection.error) : Json(nullptr)},
    };
    return json;
}

Json audio_devices_snapshot_json(AudioDevicesSnapshot const &snapshot)
{
    return Json{
        {"outputDevices", audio_device_descriptors_json(snapshot.output_devices)},
        {"inputDevices", audio_device_descriptors_json(snapshot.input_devices)},
        {"selectedOutput", audio_device_selection_json(snapshot.selected_output)},
        {"selectedInput", audio_device_selection_json(snapshot.selected_input)},
    };
}

std::string_view node_event_value_kind_json(NodeEventValueKind kind)
{
    switch (kind) {
    case NodeEventValueKind::unsupported: return "unsupported";
    case NodeEventValueKind::boolean: return "boolean";
    case NodeEventValueKind::signed_integer: return "signedInteger";
    case NodeEventValueKind::unsigned_integer: return "unsignedInteger";
    case NodeEventValueKind::floating_point: return "floatingPoint";
    case NodeEventValueKind::string: return "string";
    case NodeEventValueKind::converted_scalar: return "convertedScalar";
    case NodeEventValueKind::enumeration: return "enumeration";
    case NodeEventValueKind::aggregate: return "aggregate";
    case NodeEventValueKind::sequence: return "sequence";
    case NodeEventValueKind::node_address: return "nodeAddress";
    }
    throw std::logic_error("unknown NodeEventValueKind");
}

Json node_event_type_id_json(NodeEventTypeId const& id)
{
    return Json{
        {"nominalId", id.nominal_id},
        {"definitionFingerprint", id.definition_fingerprint},
    };
}

Json node_event_value_schema_json(NodeEventValueSchema const& schema)
{
    Json result{{"kind", node_event_value_kind_json(schema.kind)}};
    if (!schema.unsupported_reason.empty()) {
        result["reason"] = schema.unsupported_reason;
    }
    if (schema.integer_bit_width != 0) {
        result["bitWidth"] = schema.integer_bit_width;
    }
    if (schema.kind == NodeEventValueKind::enumeration) {
        result["signed"] = schema.integer_is_signed;
    }
    if (!schema.enumerators.empty()) {
        auto values = Json::array();
        for (auto const& value : schema.enumerators) {
            values.push_back(Json{
                {"name", value.name},
                {"value", value.value},
            });
        }
        result["enumerators"] = std::move(values);
    }
    if (!schema.fields.empty()) {
        auto fields = Json::array();
        for (auto const& field : schema.fields) {
            if (!field.value) {
                throw std::logic_error(
                    "NodeEventFieldSchema has no value schema");
            }
            fields.push_back(Json{
                {"name", field.name},
                {"value", node_event_value_schema_json(*field.value)},
            });
        }
        result["fields"] = std::move(fields);
    }
    if (schema.element) {
        result["element"] = node_event_value_schema_json(*schema.element);
    }
    if (schema.fixed_element_count != 0) {
        result["fixedElementCount"] = schema.fixed_element_count;
    }
    if (schema.kind == NodeEventValueKind::converted_scalar) {
        result["jsonKinds"] = Json::array({"boolean", "number", "string"});
        result["providerValidated"] = true;
    }
    return result;
}

Json node_event_protocol_json(NodeEventProtocolSnapshot const& protocol)
{
    auto event_types = Json::array();
    for (auto const& event : protocol.event_types) {
        event_types.push_back(Json{
            {"nominalId", event.id.nominal_id},
            {"definitionFingerprint", event.id.definition_fingerprint},
            {"displayName", event.display_name},
            {"payload", node_event_value_schema_json(event.payload)},
        });
    }
    auto definitions = Json::array();
    for (auto const& definition : protocol.definitions) {
        auto supported = Json::array();
        for (auto const& id : definition.supported_event_types) {
            supported.push_back(node_event_type_id_json(id));
        }
        definitions.push_back(Json{
            {"definitionId", definition.definition_id},
            {"supportedEventTypes", std::move(supported)},
        });
    }
    auto receivers = Json::array();
    for (auto const& receiver : protocol.receivers) {
        auto supported = Json::array();
        for (auto const& id : receiver.supported_event_types) {
            supported.push_back(node_event_type_id_json(id));
        }
        receivers.push_back(Json{
            {"address", node_address_json(
                receiver.address, ProjectConnectionJsonNames::socket_rpc)},
            {"supportedEventTypes", std::move(supported)},
        });
    }
    return Json{
        {"formatVersion", protocol.format_version},
        {"projectGraphGeneration", protocol.project_graph_generation},
        {"definitionsGeneration", protocol.definitions_generation},
        {"eventTypes", std::move(event_types)},
        {"definitions", std::move(definitions)},
        {"receivers", std::move(receivers)},
    };
}
} // namespace

void SocketRpcAutosaveStateResultBuilder::succeed(bool enabled, bool pending)
{
    result = enabled;
    pending_ = pending;
    error_message.clear();
}

void SocketRpcAutosaveStateResultBuilder::fail(std::string message)
{
    fail(-32000, std::move(message));
}

void SocketRpcAutosaveStateResultBuilder::fail(int code, std::string message)
{
    result.reset();
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcAutosaveStateResultBuilder::build(int request_id) const
{
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result) throw_unbuilt_response("SocketRpcAutosaveStateResultBuilder");
    return jsonrpc_result(request_id, Json{{"enabled", *result}});
}

void SocketRpcSessionProjectStateResultBuilder::succeed(
    std::vector<ProjectCommand> value)
{
    result = std::move(value);
    error_message.clear();
}

void SocketRpcSessionProjectStateResultBuilder::fail(std::string message)
{
    fail(-32000, std::move(message));
}

void SocketRpcSessionProjectStateResultBuilder::fail(
    int code, std::string message)
{
    result.reset();
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcSessionProjectStateResultBuilder::build(int request_id) const
{
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result) {
        throw_unbuilt_response("SocketRpcSessionProjectStateResultBuilder");
    }
    auto commands = Json::array();
    for (auto const& command : *result) {
        commands.push_back(Json{
            {"command", command.command},
            {"args", command.args},
        });
    }
    return jsonrpc_result(request_id, Json{{"commands", std::move(commands)}});
}

void SocketRpcNodeEventProtocolResultBuilder::succeed(
    NodeEventProtocolSnapshot value)
{
    result = std::move(value);
}

void SocketRpcNodeEventProtocolResultBuilder::fail(std::string message)
{
    fail(-32000, std::move(message));
}

void SocketRpcNodeEventProtocolResultBuilder::fail(
    int code,
    std::string message)
{
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcNodeEventProtocolResultBuilder::build(int request_id) const
{
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result) {
        throw_unbuilt_response("SocketRpcNodeEventProtocolResultBuilder");
    }
    return jsonrpc_result(request_id, node_event_protocol_json(*result));
}

void SocketRpcNodeMessageResultBuilder::succeed(
    std::vector<SocketRpcNodeMessageDiagnostic> diagnostics)
{
    diagnostics_ = std::move(diagnostics);
    has_response_ = true;
}

void SocketRpcNodeMessageResultBuilder::fail(std::string message)
{
    fail(-32000, std::move(message));
}

void SocketRpcNodeMessageResultBuilder::fail(int code, std::string message)
{
    error_code = code;
    error_message = std::move(message);
    has_response_ = true;
}

std::string SocketRpcNodeMessageResultBuilder::build(int request_id) const
{
    if (!has_response_) {
        throw_unbuilt_response("SocketRpcNodeMessageResultBuilder");
    }
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    Json diagnostics = Json::array();
    for (auto const& diagnostic : diagnostics_) {
        diagnostics.push_back(Json{
            {"target", node_address_json(
                diagnostic.target, ProjectConnectionJsonNames::socket_rpc)},
            {"message", diagnostic.message},
        });
    }
    return jsonrpc_result(request_id, Json{
        {"ok", true},
        {"diagnostics", std::move(diagnostics)},
    });
}

void SocketRpcAckResponseBuilder::succeed() noexcept {
    has_response_ = true;
}

void SocketRpcAckResponseBuilder::fail(std::string message) {
    has_response_ = true;
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcAckResponseBuilder::fail(int code, std::string message) {
    has_response_ = true;
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
    return jsonrpc_result(request_id, Json{{"nodes", symbolic_nodes_json(result->nodes)}});
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

void SocketRpcSymbolicNodeResultBuilder::succeed(SymbolicNodeInfo value) {
    result = std::move(value);
}

void SocketRpcSymbolicNodeResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcSymbolicNodeResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcSymbolicNodeResultBuilder::build(int request_id) const {
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result.has_value()) {
        throw_unbuilt_response("SocketRpcSymbolicNodeResultBuilder");
    }
    return jsonrpc_result(request_id, symbolic_node_json(*result));
}

void SocketRpcGraphScopeResultBuilder::succeed(ProjectGraphScope value) {
    result = std::move(value);
}

void SocketRpcGraphScopeResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

namespace {
Json graph_scope_ports_json(std::vector<NodeInstancePortInfo> const& ports,
    std::span<ProjectGraphScopeManualControl const> controls = {}, ManualControlKind kind = ManualControlKind::input)
{
    auto json = Json::array();
    auto const shape_name = [](auto shape) -> std::string_view {
        switch (shape) {
        case decltype(shape)::sequential: return "sequential";
        case decltype(shape)::region: return "region";
        case decltype(shape)::coverage: return "coverage";
        }
        throw std::logic_error("unknown graph scope port shape");
    };
    for (auto const& port : ports) {
        Json entry{{"name", port.name}, {"index", port.index}};
        if (!port.display_name.empty()) entry["displayName"] = port.display_name;
        if (port.access) entry["access"] = shape_name(*port.access);
        if (port.production) entry["production"] = shape_name(*port.production);
        if (port.realization) entry["realization"] = *port.realization == OutputRealization::buffered ? "buffered" : "computed";
        if (port.channel_type) {
            entry["channelType"] = std::string(channel_type_info(*port.channel_type).wire_name);
            entry["valueTypes"] = continuous_value_types_json(port.value_types);
        }
        if (port.event_type) {
            entry["eventType"] = std::string(details::event_type_name(*port.event_type));
        }
        auto control = std::ranges::find_if(controls, [&](auto const& value) {
            return value.control.id.kind == kind && value.control.id.port_name == port.name;
        });
        if (control != controls.end()) entry["manualControl"] = Json{
            {"value", control->control.stored_value.value}, {"enabled", control->control.enabled},
            {"participation", control->control.participation == ManualControlParticipation::disconnected_only
                ? "disconnectedOnly" : "participateWithConnections"},
            {"available", control->available}, {"participates", control->participates},
            {"live", control->live_value.has_value()},
        };
        if (control != controls.end() && control->live_value) {
            entry["manualControl"]["liveValue"] = control->live_value->value;
        }
        json.push_back(std::move(entry));
    }
    return json;
}

std::string_view graph_scope_node_state_json(ProjectGraphScopeNodeState state)
{
    switch (state) {
    case ProjectGraphScopeNodeState::waiting_for_definition: return "waitingForDefinition";
    case ProjectGraphScopeNodeState::waiting_for_graph: return "waitingForGraph";
    case ProjectGraphScopeNodeState::ready: return "ready";
    case ProjectGraphScopeNodeState::failed: return "failed";
    }
    throw std::logic_error("unknown graph scope node state");
}

void preserve_construction_integers(Json& value)
{
    constexpr std::int64_t max_exact_integer = 9007199254740991;
    if (value.is_structured()) {
        for (auto& member : value) preserve_construction_integers(member);
    } else if (value.is_number_unsigned()) {
        auto const integer = value.get<std::uint64_t>();
        if (integer > static_cast<std::uint64_t>(max_exact_integer)) value = std::to_string(integer);
    } else if (value.is_number_integer()) {
        auto const integer = value.get<std::int64_t>();
        if (integer < -max_exact_integer || integer > max_exact_integer) value = std::to_string(integer);
    }
}

Json graph_scope_construction_json(ProjectGraphScopeNode const& node)
{
    if (!node.construction_arguments_json) return nullptr;
    auto arguments = Json::parse(*node.construction_arguments_json);
    preserve_construction_integers(arguments);
    return {{"definitionId", node.construction_definition_id}, {"arguments", std::move(arguments)}};
}

Json graph_scope_nodes_json(std::span<ProjectGraphScopeNode const> entries)
{
    auto nodes = Json::array();
    for (auto const& node : entries) {
        Json entry{
            {"address", node_address_json(node.address, ProjectConnectionJsonNames::socket_rpc)},
            {"instance", iv_module_instance_json(node.instance)},
            {"definitionId", node.construction_definition_id.empty()
                ? node.instance.definition_id : node.construction_definition_id},
            {"sampleInputs", graph_scope_ports_json(node.interface.sample_inputs, node.manual_controls)},
            {"sampleOutputs", graph_scope_ports_json(node.interface.sample_outputs, node.manual_controls, ManualControlKind::output_gain)},
            {"eventInputs", graph_scope_ports_json(node.interface.event_inputs)},
            {"eventOutputs", graph_scope_ports_json(node.interface.event_outputs)},
            {"hasChildren", node.interface.has_children},
            {"isModule", node.interface.is_module},
            {"errorMessage", node.error_message},
            {"loadState", graph_scope_node_state_json(node.load_state)},
            {"construction", graph_scope_construction_json(node)},
            {"constructionError", node.construction_error},
        };
        if (node.membership_address)
            entry["membershipAddress"] = node_address_json(*node.membership_address, ProjectConnectionJsonNames::socket_rpc);
        if (node.symbolic_node) entry["symbolicNode"] = true;
        if (node.symbolic_member_count) entry["symbolicMemberCount"] = *node.symbolic_member_count;
        if (node.child_display_name) entry["childDisplayName"] = *node.child_display_name;
        if (node.child_source_label) entry["childSourceLabel"] = *node.child_source_label;
        if (node.declaration_source) {
            entry["declarationSource"] = {
                {"filePath", node.declaration_source->file_path},
                {"line", node.declaration_source->line},
                {"byteOffset", node.declaration_source->byte_offset},
            };
        }
        nodes.push_back(std::move(entry));
    }
    return nodes;
}
} // namespace

std::string SocketRpcGraphScopeResultBuilder::build(int request_id) const {
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result.has_value()) {
        throw_unbuilt_response("SocketRpcGraphScopeResultBuilder");
    }
    auto child_scopes = Json::array();
    for (auto const& scope : result->child_scopes) {
        child_scopes.push_back({
            {"subject", node_address_json(scope.subject, ProjectConnectionJsonNames::socket_rpc)},
            {"nodes", graph_scope_nodes_json(scope.nodes)},
            {"hasBoundary", scope.has_boundary},
        });
    }
    auto connections = Json::array();
    for (auto const& entry : result->connections) {
        connections.push_back({
            {"id", project_connection_id(entry.connection)},
            {"connection", project_connection_json(
                entry.connection, ProjectConnectionJsonNames::socket_rpc)},
            {"applied", entry.applied},
            {"sourceOwned", entry.source_owned},
        });
    }
    auto timelines = Json::array();
    for (auto const& address : result->timeline_nodes)
        timelines.push_back(node_address_json(address, ProjectConnectionJsonNames::socket_rpc));
    auto lane_order = Json::array();
    auto lane_views = Json::array();
    auto unavailable_lane_parents = Json::array();
    constexpr auto names = ProjectConnectionJsonNames::socket_rpc;
    for (auto const& address : result->unavailable_lane_parents)
        unavailable_lane_parents.push_back(node_address_json(address, names));
    for (auto const& address : result->presentation.lane_order)
        lane_order.push_back(node_address_json(address, names));
    for (auto const& view : result->presentation.lane_views)
        lane_views.push_back(project_lane_view_json(view, names));
    return jsonrpc_result(request_id, Json{
        {"generation", result->generation ? Json(*result->generation) : Json(nullptr)},
        {"sampleRate", result->sample_rate ? Json(*result->sample_rate) : Json(nullptr)},
        {"execution", {{"phase", result->execution_phase}, {"message", result->execution_message},
            {"preparedGeneration", result->prepared_generation ? Json(*result->prepared_generation) : Json(nullptr)},
            {"activeGeneration", result->active_generation ? Json(*result->active_generation) : Json(nullptr)}}},
        {"editable", result->editable},
        {"timelineNodes", std::move(timelines)},
        {"unavailableLaneParents", std::move(unavailable_lane_parents)},
        {"presentation", {{"laneOrder", std::move(lane_order)}, {"laneViews", std::move(lane_views)},
            {"laneParents", project_lane_parents_json(result->presentation.lane_parents, names)},
            {"timelineSnapMemory", Json::parse(result->presentation.timeline_snap_memory_json)}}},
        {"nodes", graph_scope_nodes_json(result->nodes)},
        {"childScopes", std::move(child_scopes)},
        {"connections", std::move(connections)},
    });
}

void SocketRpcSymbolicNodesResultBuilder::succeed(std::vector<SymbolicNodeInfo> value) {
    result = std::move(value);
}

void SocketRpcSymbolicNodesResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcSymbolicNodesResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcSymbolicNodesResultBuilder::build(int request_id) const {
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result.has_value()) {
        throw_unbuilt_response("SocketRpcSymbolicNodesResultBuilder");
    }
    return jsonrpc_result(request_id, Json{{"nodes", symbolic_nodes_json(*result)}});
}

void SocketRpcCreateNodeInstanceResultBuilder::succeed(std::string created_instance_id) {
    instance_id = std::move(created_instance_id);
}

void SocketRpcCreateNodeInstanceResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcCreateNodeInstanceResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

void SocketRpcIvPackageDefinitionsResultBuilder::succeed(std::vector<IvPackageInfo> value) {
    result = std::move(value);
}

void SocketRpcIvPackageDefinitionsResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcIvPackageDefinitionsResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcIvPackageDefinitionsResultBuilder::build(int request_id) const {
    if (!error_message.empty()) return jsonrpc_error(request_id, error_code, error_message);
    if (!result.has_value()) throw_unbuilt_response("SocketRpcIvPackageDefinitionsResultBuilder");
    return jsonrpc_result(request_id, Json{{"packages", iv_package_definitions_json(*result)}});
}

void SocketRpcIvPackageResultBuilder::succeed(IvPackageInfo value) {
    result = std::move(value);
}

void SocketRpcIvPackageResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcIvPackageResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcIvPackageResultBuilder::build(int request_id) const {
    if (!error_message.empty()) return jsonrpc_error(request_id, error_code, error_message);
    if (!result.has_value()) throw_unbuilt_response("SocketRpcIvPackageResultBuilder");
    return jsonrpc_result(request_id, Json{{"package", iv_package_json(*result)}});
}

std::string SocketRpcCreateNodeInstanceResultBuilder::build(int request_id) const {
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!instance_id.has_value()) {
        throw_unbuilt_response("SocketRpcCreateNodeInstanceResultBuilder");
    }
    return jsonrpc_result(request_id, Json{{"instanceId", *instance_id}});
}

void SocketRpcNodeInstancesResultBuilder::succeed(std::vector<IvModuleInstanceInfo> value) {
    result = std::move(value);
}

void SocketRpcNodeInstancesResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcNodeInstancesResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcNodeInstancesResultBuilder::build(int request_id) const {
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result.has_value()) {
        throw_unbuilt_response("SocketRpcNodeInstancesResultBuilder");
    }
    return jsonrpc_result(request_id, Json{{"instances", iv_module_instances_json(*result)}});
}

void SocketRpcAudioDevicesResultBuilder::succeed(AudioDevicesSnapshot value)
{
    result = std::move(value);
}

void SocketRpcAudioDevicesResultBuilder::fail(std::string message)
{
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcAudioDevicesResultBuilder::fail(int code, std::string message)
{
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcAudioDevicesResultBuilder::build(int request_id) const
{
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result.has_value()) {
        throw_unbuilt_response("SocketRpcAudioDevicesResultBuilder");
    }
    return jsonrpc_result(request_id, audio_devices_snapshot_json(*result));
}

} // namespace iv
