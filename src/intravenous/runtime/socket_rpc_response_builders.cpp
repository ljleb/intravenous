#include <intravenous/runtime/socket_rpc_response_builders.h>
#include <intravenous/runtime/socket_rpc_json_serialization.h>
#include <intravenous/runtime/project_connection_json.h>

#include <nlohmann/json.hpp>

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
    return jsonrpc_result(request_id, Json{{"nodes", virtual_nodes_json(result->nodes)}});
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

void SocketRpcVirtualNodeResultBuilder::succeed(VirtualNodeInfo value) {
    result = std::move(value);
}

void SocketRpcVirtualNodeResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcVirtualNodeResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcVirtualNodeResultBuilder::build(int request_id) const {
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result.has_value()) {
        throw_unbuilt_response("SocketRpcVirtualNodeResultBuilder");
    }
    return jsonrpc_result(request_id, virtual_node_json(*result));
}

void SocketRpcVirtualNodesResultBuilder::succeed(std::vector<VirtualNodeInfo> value) {
    result = std::move(value);
}

void SocketRpcVirtualNodesResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcVirtualNodesResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcVirtualNodesResultBuilder::build(int request_id) const {
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result.has_value()) {
        throw_unbuilt_response("SocketRpcVirtualNodesResultBuilder");
    }
    return jsonrpc_result(request_id, Json{{"nodes", virtual_nodes_json(*result)}});
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
