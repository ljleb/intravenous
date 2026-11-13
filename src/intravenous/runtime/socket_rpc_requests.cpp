#include <intravenous/runtime/socket_rpc_requests.h>

#include <intravenous/runtime/project_connection_json.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace iv {
namespace {
using Json = nlohmann::ordered_json;

Json parse_request_json(std::string_view line) {
    try {
        return Json::parse(line);
    } catch (Json::parse_error const &e) {
        throw std::runtime_error(std::string("invalid JSON-RPC request: ") + e.what());
    }
}

Json const &parse_request_params(Json const &request) {
    auto const params_it = request.find("params");
    if (params_it == request.end() || !params_it->is_object()) {
        throw std::runtime_error("JSON-RPC request is missing params object");
    }
    return *params_it;
}

int parse_request_id(Json const &request) {
    auto const id_it = request.find("id");
    if (id_it == request.end() || !id_it->is_number_integer()) {
        throw std::runtime_error("JSON-RPC request is missing numeric id");
    }
    return id_it->get<int>();
}

std::string parse_request_method(Json const &request) {
    auto const method_it = request.find("method");
    if (method_it == request.end() || !method_it->is_string()) {
        throw std::runtime_error("JSON-RPC request is missing method");
    }
    return method_it->get<std::string>();
}

std::string parse_string_param(Json const &params, std::string const &key) {
    auto const value_it = params.find(key);
    if (value_it == params.end() || !value_it->is_string()) {
        throw std::runtime_error("JSON-RPC request is missing string param '" + key + "'");
    }
    return value_it->get<std::string>();
}

std::optional<std::string> parse_optional_nullable_string_param(
    Json const &params,
    std::string const &key)
{
    auto const value_it = params.find(key);
    if (value_it == params.end() || value_it->is_null()) {
        return std::nullopt;
    }
    if (!value_it->is_string()) {
        throw std::runtime_error("JSON-RPC request param '" + key + "' must be a string or null");
    }
    return value_it->get<std::string>();
}

std::vector<std::string> parse_string_array_param(Json const &params, std::string const &key) {
    auto const value_it = params.find(key);
    if (value_it == params.end() || !value_it->is_array()) {
        throw std::runtime_error("JSON-RPC request is missing string array param '" + key + "'");
    }

    std::vector<std::string> values;
    values.reserve(value_it->size());
    for (auto const &item : *value_it) {
        if (!item.is_string()) {
            throw std::runtime_error("JSON-RPC request param '" + key + "' must be an array of strings");
        }
        values.push_back(item.get<std::string>());
    }
    return values;
}

uint32_t parse_uint32_value(Json const &value, std::string const &context) {
    if (!value.is_number_integer()) {
        throw std::runtime_error(context);
    }
    auto const parsed = value.get<int64_t>();
    if (parsed < 0) {
        throw std::runtime_error(context);
    }
    return static_cast<uint32_t>(parsed);
}

std::optional<std::uint64_t> parse_optional_revision(Json const& params)
{
    auto const value = params.find("expectedRevision");
    if (value == params.end() || value->is_null()) return std::nullopt;
    if (!value->is_number_unsigned() && !value->is_number_integer()) {
        throw std::runtime_error("expectedRevision must be an unsigned integer");
    }
    if (value->is_number_unsigned()) return value->get<std::uint64_t>();
    auto const parsed = value->get<std::int64_t>();
    if (parsed < 0) {
        throw std::runtime_error("expectedRevision must be an unsigned integer");
    }
    return static_cast<std::uint64_t>(parsed);
}

std::uint64_t parse_revision_param(
    Json const& params, std::string const& key, std::uint64_t fallback)
{
    auto const value = params.find(key);
    if (value == params.end()) return fallback;
    if (!value->is_number_unsigned() && !value->is_number_integer()) {
        throw std::runtime_error(key + " must be an unsigned integer");
    }
    if (value->is_number_unsigned()) return value->get<std::uint64_t>();
    auto const parsed = value->get<std::int64_t>();
    if (parsed < 0) {
        throw std::runtime_error(key + " must be an unsigned integer");
    }
    return static_cast<std::uint64_t>(parsed);
}

std::vector<SourceRange> parse_ranges(Json const &params, bool require_non_empty = true) {
    std::vector<SourceRange> ranges;
    auto const ranges_it = params.find("ranges");
    if (ranges_it != params.end()) {
        if (!ranges_it->is_array()) {
            throw std::runtime_error("graph.queryBySpans ranges must be an array");
        }
        ranges.reserve(ranges_it->size());
        for (auto const &range_json : *ranges_it) {
            if (!range_json.is_object()) {
                throw std::runtime_error("graph.queryBySpans range entries must be objects");
            }
            auto const start_it = range_json.find("start");
            auto const end_it = range_json.find("end");
            if (start_it == range_json.end() || end_it == range_json.end() ||
                !start_it->is_object() || !end_it->is_object()) {
                throw std::runtime_error("graph.queryBySpans ranges must include start and end positions");
            }

            auto const start_line_it = start_it->find("line");
            auto const start_column_it = start_it->find("column");
            auto const end_line_it = end_it->find("line");
            auto const end_column_it = end_it->find("column");
            if (start_line_it == start_it->end() ||
                start_column_it == start_it->end() ||
                end_line_it == end_it->end() ||
                end_column_it == end_it->end()) {
                throw std::runtime_error("graph.queryBySpans positions must use unsigned line/column values");
            }

            ranges.push_back(SourceRange{
                .start = {
                    .line = parse_uint32_value(*start_line_it, "graph.queryBySpans positions must use unsigned line/column values"),
                    .column = parse_uint32_value(*start_column_it, "graph.queryBySpans positions must use unsigned line/column values"),
                },
                .end = {
                    .line = parse_uint32_value(*end_line_it, "graph.queryBySpans positions must use unsigned line/column values"),
                    .column = parse_uint32_value(*end_column_it, "graph.queryBySpans positions must use unsigned line/column values"),
                },
            });
        }
    }
    if (require_non_empty && ranges.empty()) {
        throw std::runtime_error("graph.queryBySpans requires at least one range");
    }
    return ranges;
}

SourceRangeMatchMode parse_match_mode(Json const &params) {
    auto const mode_it = params.find("match");
    if (mode_it == params.end()) {
        return SourceRangeMatchMode::intersection;
    }
    if (!mode_it->is_string()) {
        throw std::runtime_error("graph.queryBySpans match must be 'union' or 'intersection'");
    }
    auto const mode = mode_it->get<std::string>();
    if (mode == "union") {
        return SourceRangeMatchMode::union_;
    }
    if (mode == "intersection") {
        return SourceRangeMatchMode::intersection;
    }
    throw std::runtime_error("graph.queryBySpans match must be 'union' or 'intersection'");
}

ManualControlId parse_manual_control_id(Json const& params)
{
    auto const node = params.find("node");
    if (node == params.end() || !node->is_object()) {
        throw std::runtime_error(
            "manual control request requires object param 'node'");
    }
    return {
        .node = parse_node_address_json(
            *node, ProjectConnectionJsonNames::socket_rpc),
        .input_name = parse_string_param(params, "inputName"),
    };
}

Sample parse_manual_control_value(Json const& params)
{
    auto const value = params.find("value");
    if (value == params.end() || !value->is_number()) {
        throw std::runtime_error(
            "manual control request requires numeric param 'value'");
    }
    return value->get<float>();
}

ManualControlInteractionId parse_manual_interaction_id(Json const& params)
{
    return {
        .interaction_id = parse_string_param(params, "interactionId"),
    };
}

} // namespace

ParsedSocketRpcRequest parse_socket_rpc_request(std::string_view line)
{
    auto const request = parse_request_json(line);
    auto const request_id = parse_request_id(request);
    auto const method = parse_request_method(request);
    auto const &params = parse_request_params(request);
    auto const expected_revision = parse_optional_revision(params);

    auto parsed = [&](SocketRpcRequestPayload payload) {
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .method = method,
            .expected_revision = expected_revision,
            .payload = std::move(payload),
        };
    };

    if (method == "graph.queryBySpans") {
        return parsed(GraphQueryBySpansRequest{
            .file_path = parse_string_param(params, "filePath"),
            .ranges = parse_ranges(params),
            .match_mode = parse_match_mode(params),
            .instance_id = parse_optional_nullable_string_param(params, "instanceId")});
    }
    if (method == "graph.queryActiveRegions") {
        return parsed(GraphQueryActiveRegionsRequest{
            .file_path = parse_string_param(params, "filePath")});
    }
    if (method == "graph.getVirtualNode") {
        return parsed(GetVirtualNodeRequest{
            .node_id = parse_string_param(params, "nodeId")});
    }
    if (method == "graph.getVirtualNodes") {
        return parsed(GetVirtualNodesRequest{
            .node_ids = parse_string_array_param(params, "nodeIds")});
    }
    if (method == "graph.getScope") {
        return parsed(GetGraphScopeRequest{});
    }
    if (method == "nodeInstances.create") {
        auto configuration_source = parse_optional_nullable_string_param(
            params, "configurationSource");
        return parsed(CreateNodeInstanceRequest{
            .instance_id = parse_optional_nullable_string_param(params, "instanceId"),
            .definition_id = parse_string_param(params, "definitionId"),
            .display_name = parse_optional_nullable_string_param(params, "displayName"),
            .configuration_source = configuration_source.value_or(std::string{}),
        });
    }
    if (method == "ivPackages.list") {
        return parsed(GetIvPackageDefinitionsRequest{});
    }
    if (method == "ivPackages.create") {
        return parsed(CreateIvPackageRequest{
            .name = parse_string_param(params, "name")});
    }
    if (method == "nodeInstances.get") {
        auto const source_file_path = parse_optional_nullable_string_param(params, "sourceFilePath");
        return parsed(GetNodeInstancesRequest{
            .source_file_path = source_file_path.has_value()
                ? std::optional<std::filesystem::path>{*source_file_path}
                : std::nullopt});
    }
    if (method == "nodeInstances.delete") {
        return parsed(DeleteNodeInstanceRequest{
            .instance_id = parse_string_param(params, "instanceId")});
    }
    if (method == "nodeInstances.update") {
        auto const updates_it = params.find("updates");
        if (updates_it == params.end() || !updates_it->is_array()) {
            throw std::runtime_error("socket rpc params are missing array 'updates'");
        }
        std::vector<UpdateNodeInstance> updates;
        updates.reserve(updates_it->size());
        for (auto const &update : *updates_it) {
            if (!update.is_object()) {
                throw std::runtime_error("socket rpc update entry must be an object");
            }
            updates.push_back(UpdateNodeInstance{
                .instance_id = parse_string_param(update, "instanceId"),
                .display_name = parse_optional_nullable_string_param(update, "displayName"),
                .configuration_source = parse_optional_nullable_string_param(
                    update, "configurationSource"),
            });
        }
        return parsed(UpdateNodeInstancesRequest{
            .updates = std::move(updates)});
    }
    if (method == "graphConnections.upsert") {
        auto const connection = params.find("connection");
        if (connection == params.end() || !connection->is_object()) {
            throw std::runtime_error(
                "graphConnections.upsert requires object param 'connection'");
        }
        return parsed(UpsertGraphConnectionRequest{
                .connection = parse_project_connection_json(
                    *connection, ProjectConnectionJsonNames::socket_rpc),
            });
    }
    if (method == "graphConnections.delete") {
        return parsed(DeleteGraphConnectionRequest{
                .connection_id = parse_string_param(params, "connectionId"),
            });
    }
    if (method == "manualControls.upsert") {
        auto const participation = parse_string_param(params, "participation");
        ManualControlParticipation parsed_participation{};
        if (participation == "disconnectedOnly") {
            parsed_participation =
                ManualControlParticipation::disconnected_only;
        } else if (participation == "participateWithConnections") {
            parsed_participation =
                ManualControlParticipation::participate_with_connections;
        } else {
            throw std::runtime_error(
                "manual control participation must be 'disconnectedOnly' or 'participateWithConnections'");
        }
        auto const enabled = params.find("enabled");
        if (enabled != params.end() && !enabled->is_boolean()) {
            throw std::runtime_error(
                "manual control param 'enabled' must be boolean");
        }
        return parsed(UpsertManualControlRequest{
            .control = {
                .id = parse_manual_control_id(params),
                .stored_value = parse_manual_control_value(params),
                .participation = parsed_participation,
                .enabled = enabled == params.end() || enabled->get<bool>(),
            },
        });
    }
    if (method == "manualControls.delete") {
        return parsed(DeleteManualControlRequest{
            .id = parse_manual_control_id(params),
        });
    }
    if (method == "manualControls.hover") {
        auto const node = params.find("node");
        auto const input = params.find("inputName");
        auto id = std::optional<ManualControlId>{};
        if ((node == params.end()) != (input == params.end())) {
            throw std::runtime_error(
                "manual control hover requires both 'node' and 'inputName', or neither");
        }
        if (node != params.end()) id = parse_manual_control_id(params);
        return parsed(SetManualControlHoverRequest{
            .id = std::move(id),
        });
    }
    if (method == "manualControls.begin") {
        return parsed(BeginManualControlInteractionRequest{
            .interaction = parse_manual_interaction_id(params),
            .id = parse_manual_control_id(params),
            .value = parse_manual_control_value(params),
        });
    }
    if (method == "manualControls.update") {
        return parsed(UpdateManualControlInteractionRequest{
            .interaction = parse_manual_interaction_id(params),
            .value = parse_manual_control_value(params),
        });
    }
    if (method == "manualControls.end") {
        return parsed(EndManualControlInteractionRequest{
            .interaction = parse_manual_interaction_id(params),
            .value = parse_manual_control_value(params),
        });
    }
    if (method == "nodeMessages.getProtocol") {
        return parsed(GetNodeEventProtocolRequest{});
    }
    if (method == "nodeMessages.dispatch") {
        auto const target = params.find("target");
        if (target == params.end() || !target->is_object()) {
            throw std::runtime_error(
                "nodeMessages.dispatch requires object param 'target'");
        }
        auto const payload = params.find("payload");
        if (payload == params.end()) {
            throw std::runtime_error(
                "nodeMessages.dispatch requires param 'payload'");
        }
        return parsed(DispatchNodeMessageRequest{
                .target = parse_node_address_json(
                    *target, ProjectConnectionJsonNames::socket_rpc),
                .event_type_nominal_id = parse_string_param(
                    params, "eventTypeNominalId"),
                .event_type_definition_fingerprint = parse_string_param(
                    params, "eventTypeDefinitionFingerprint"),
                .payload = std::make_shared<Json>(*payload),
            });
    }
    if (method == "audioDevices.get") {
        return parsed(GetAudioDevicesRequest{});
    }
    if (method == "audioDevices.set") {
        return parsed(SetAudioDevicesRequest{
            .output_device_id = parse_optional_nullable_string_param(params, "outputDeviceId"),
            .input_device_id = parse_optional_nullable_string_param(params, "inputDeviceId")});
    }
    if (method == "project.save") {
        return parsed(SaveProjectRequest{});
    }
    if (method == "project.enableAutosave") {
        return parsed(EnableProjectAutosaveRequest{});
    }
    if (method == "project.disableAutosave") {
        return parsed(DisableProjectAutosaveRequest{});
    }
    if (method == "session.hello") {
        return parsed(SessionHelloRequest{
            .client_id = parse_string_param(params, "clientId"),
            .local_capability = parse_string_param(params, "localCapability"),
        });
    }
    if (method == "session.subscribe") {
        auto topics = std::vector<std::string>{};
        if (params.contains("topics")) {
            topics = parse_string_array_param(params, "topics");
        }
        return parsed(SessionSubscribeRequest{
            .after_revision = parse_revision_param(params, "afterRevision", 0),
            .topics = std::move(topics),
        });
    }
    if (method == "session.snapshot") {
        return parsed(SessionSnapshotRequest{});
    }
    if (method == "server.shutdown") {
        return parsed(ServerShutdownRequest{});
    }

    return parsed(UnsupportedSocketRpcRequest{.method = method});
}
} // namespace iv
