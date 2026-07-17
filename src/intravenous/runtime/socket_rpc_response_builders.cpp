#include <intravenous/runtime/socket_rpc_response_builders.h>
#include <intravenous/runtime/socket_rpc_json_serialization.h>

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

void SocketRpcIvModuleSourcesResultBuilder::succeed(std::vector<IvModuleSourceInfo> value) {
    result = std::move(value);
}

void SocketRpcIvModuleSourcesResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcIvModuleSourcesResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcIvModuleSourcesResultBuilder::build(int request_id) const {
    if (!error_message.empty()) return jsonrpc_error(request_id, error_code, error_message);
    if (!result.has_value()) throw_unbuilt_response("SocketRpcIvModuleSourcesResultBuilder");
    return jsonrpc_result(request_id, Json{{"sources", iv_module_sources_json(*result)}});
}

void SocketRpcIvModuleSourceResultBuilder::succeed(IvModuleSourceInfo value) {
    result = std::move(value);
}

void SocketRpcIvModuleSourceResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcIvModuleSourceResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcIvModuleSourceResultBuilder::build(int request_id) const {
    if (!error_message.empty()) return jsonrpc_error(request_id, error_code, error_message);
    if (!result.has_value()) throw_unbuilt_response("SocketRpcIvModuleSourceResultBuilder");
    return jsonrpc_result(request_id, Json{{"source", iv_module_source_json(*result)}});
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

void SocketRpcIvModuleInstancesResultBuilder::succeed(std::vector<IvModuleInstanceInfo> value) {
    result = std::move(value);
}

void SocketRpcIvModuleInstancesResultBuilder::fail(std::string message) {
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcIvModuleInstancesResultBuilder::fail(int code, std::string message) {
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcIvModuleInstancesResultBuilder::build(int request_id) const {
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result.has_value()) {
        throw_unbuilt_response("SocketRpcIvModuleInstancesResultBuilder");
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

void SocketRpcLaneQuerySchemaResultBuilder::succeed(query::LaneQuerySchema value)
{
    result = std::move(value);
}

void SocketRpcLaneQuerySchemaResultBuilder::fail(std::string message)
{
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcLaneQuerySchemaResultBuilder::fail(int code, std::string message)
{
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcLaneQuerySchemaResultBuilder::build(int request_id) const
{
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result.has_value()) {
        throw_unbuilt_response("SocketRpcLaneQuerySchemaResultBuilder");
    }
    return jsonrpc_result(request_id, lane_query_schema_json(*result));
}

void SocketRpcLaneQueryCompletionResultBuilder::succeed(
    query::LaneQueryCompletionResult value,
    std::uint64_t revision)
{
    result = std::move(value);
    schema_revision = revision;
}

void SocketRpcLaneQueryCompletionResultBuilder::fail(std::string message)
{
    error_code = -32000;
    error_message = std::move(message);
}

void SocketRpcLaneQueryCompletionResultBuilder::fail(int code, std::string message)
{
    error_code = code;
    error_message = std::move(message);
}

std::string SocketRpcLaneQueryCompletionResultBuilder::build(int request_id) const
{
    if (!error_message.empty()) {
        return jsonrpc_error(request_id, error_code, error_message);
    }
    if (!result.has_value()) {
        throw_unbuilt_response("SocketRpcLaneQueryCompletionResultBuilder");
    }
    return jsonrpc_result(
        request_id,
        lane_query_completion_json(*result, schema_revision));
}

std::string SocketRpcLaneTypesResultBuilder::build(int request_id) const {
    if (!error_message.empty()) return jsonrpc_error(request_id, error_code, error_message);
    if (!result.has_value()) throw_unbuilt_response("SocketRpcLaneTypesResultBuilder");
    Json types = Json::array();
    for (auto const& type : *result) types.push_back(Json{{"typeId", type.type_id},
        {"category", type.category}, {"label", type.label}, {"description", type.description}});
    return jsonrpc_result(request_id, Json{{"laneTypes", std::move(types)}});
}
} // namespace iv
