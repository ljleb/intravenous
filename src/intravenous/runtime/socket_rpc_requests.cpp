#include <intravenous/runtime/socket_rpc_requests.h>

#include <intravenous/runtime/project_connection_json.h>
#include <intravenous/runtime/project_presentation_json.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <charconv>
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
    if (params.contains("inputName") && params.contains("outputName"))
        throw std::runtime_error("manual control request must select one input or output");
    auto const output_gain = params.contains("outputName");
    return {
        .node = parse_node_address_json(
            *node, ProjectConnectionJsonNames::socket_rpc),
        .port_name = parse_string_param(params, output_gain ? "outputName" : "inputName"),
        .kind = output_gain ? ManualControlKind::output_gain : ManualControlKind::input,
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

std::optional<LiveManualControlUpdateRequest>
parse_live_manual_control_update(std::string_view line)
{
    auto const request = parse_request_json(line);
    if (!request.is_object() || request.contains("id")) return std::nullopt;
    auto const method = request.find("method");
    if (method == request.end() || !method->is_string()
        || method->get_ref<std::string const&>() != "manualControls.update") {
        return std::nullopt;
    }
    auto const& params = parse_request_params(request);
    auto value = parse_manual_control_value(params);
    if (!std::isfinite(value.value)) {
        throw std::runtime_error("manual control update requires a finite 'value'");
    }
    return LiveManualControlUpdateRequest{
        .interaction = parse_manual_interaction_id(params),
        .value = value,
    };
}

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
        GetGraphScopeRequest query;
        if (auto const expanded = params.find("expanded"); expanded != params.end()) {
            if (!expanded->is_array()) throw std::invalid_argument("expanded must be an array of node addresses");
            for (auto const& address : *expanded)
                query.expanded.push_back(parse_node_address_json(address, ProjectConnectionJsonNames::socket_rpc));
        }
        return parsed(std::move(query));
    }
    if (method == "nodeDefinitions.search") {
        auto const& params = parse_request_params(request);
        auto source = parse_string_param(params, "source");
        std::optional<std::size_t> cursor;
        if (auto const value = params.find("cursor"); value != params.end()) {
            cursor = parse_uint32_value(*value, "cursor must be an unsigned UTF-8 byte offset");
        }
        if (source.size() > 1024 || (cursor ? *cursor > source.size() : !source.empty()))
            throw std::runtime_error("invalid node definition query length or cursor");
        return parsed(SearchNodeDefinitionsRequest{std::move(source), cursor});
    }
    if (method == "project.setToolchain") {
        auto const settings = params.find("toolchain");
        if (settings == params.end() || !settings->is_object())
            throw std::invalid_argument("project.setToolchain requires a toolchain object");
        auto path = [&](char const* name) -> std::optional<std::filesystem::path> {
            auto value = parse_optional_nullable_string_param(*settings, name);
            return value ? std::optional<std::filesystem::path>(*value) : std::nullopt;
        };
        return parsed(SetProjectToolchainRequest{{
            .c_compiler = path("cCompiler"), .cxx_compiler = path("cxxCompiler"),
            .cmake_program = path("cmakeProgram"),
            .cmake_generator = parse_optional_nullable_string_param(*settings, "cmakeGenerator"),
            .make_program = path("makeProgram"), .juce_dir = path("juceDir"),
            .iv_package_pch = path("ivPackagePch"),
        }});
    }
    if (method == "project.undo") return parsed(UndoProjectEditRequest{});
    if (method == "project.redo") return parsed(RedoProjectEditRequest{});
    if (method == "project.applyEdit") {
        ProjectEditBatch batch;
        auto const operations = params.find("operations");
        if (operations == params.end() || !operations->is_array())
            throw std::invalid_argument("project.applyEdit requires an operations array");
        for (auto const& operation : *operations) {
            if (!operation.is_object()) throw std::invalid_argument("project batch operation must be an object");
            auto const operation_method = parse_string_param(operation, "method");
            if (operation_method == "project.applyEdit" || operation_method == "project.undo" || operation_method == "project.redo")
                throw std::invalid_argument("project batches cannot contain history or nested batches");
            auto const operation_params = operation.value("params", Json::object());
            constexpr auto names = ProjectConnectionJsonNames::socket_rpc;
            if (operation_method == "projectPresentation.setLaneOrder") {
                if (batch.lane_order) throw std::invalid_argument("project batch edits lane order twice");
                batch.lane_order = parse_project_lane_order(operation_params.at("order"), names);
                continue;
            }
            if (operation_method == "projectPresentation.setLaneView") {
                auto key = parse_project_lane_view_id(operation_params, names);
                auto const& state = operation_params.at("state");
                std::optional<ProjectLaneView> view;
                if (!state.is_null()) view = ProjectLaneView{key, encode_project_presentation_object(state)};
                batch.lane_views.push_back({std::move(key), std::move(view)});
                continue;
            }
            if (operation_method == "projectPresentation.setTimelineSnapMemory") {
                if (batch.timeline_snap_memory_json) throw std::invalid_argument("project batch edits snap memory twice");
                batch.timeline_snap_memory_json = encode_project_presentation_object(operation_params.at("memory"));
                continue;
            }
            if (operation_method == "nodeInstances.duplicate") {
                auto const copies = operation_params.find("copies");
                if (copies == operation_params.end() || !copies->is_array() || copies->empty())
                    throw std::invalid_argument("nodeInstances.duplicate requires a nonempty copies array");
                for (auto const& copy : *copies) {
                    batch.instance_duplicates.push_back({
                        .source_instance_id = parse_string_param(copy, "sourceInstanceId"),
                        .instance_id = parse_string_param(copy, "instanceId"),
                    });
                }
                continue;
            }
            auto const nested = parse_socket_rpc_request(Json{{"id", 0}, {"method", operation_method},
                {"params", operation_params}}.dump());
            std::visit([&](auto const& value) {
                using Request = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<Request, CreateNodeInstanceRequest>) {
                    if (!value.instance_id) throw std::invalid_argument("batch instance creation requires an explicit instanceId");
                    batch.require_absent_instances.push_back(*value.instance_id);
                    batch.instances.push_back({*value.instance_id, ProjectNodeInstanceDeclaration{
                        .instance_id = *value.instance_id, .definition_id = value.definition_id,
                        .display_name = value.display_name, .configuration_source = value.configuration_source}});
                } else if constexpr (std::same_as<Request, UpdateNodeInstancesRequest>) {
                    for (auto const& update : value.updates) batch.instance_updates.push_back({
                        update.instance_id, update.display_name, update.configuration_source});
                } else if constexpr (std::same_as<Request, DeleteNodeInstanceRequest>) {
                    batch.instances.push_back({value.instance_id, std::nullopt});
                } else if constexpr (std::same_as<Request, UpsertGraphConnectionRequest>) {
                    batch.connections.push_back({project_connection_id(value.connection), value.connection});
                } else if constexpr (std::same_as<Request, DeleteGraphConnectionRequest>) {
                    batch.connections.push_back({value.connection_id, std::nullopt, true});
                } else if constexpr (std::same_as<Request, UpsertManualControlRequest>) {
                    batch.manual_controls.push_back({value.control.id, value.control});
                } else if constexpr (std::same_as<Request, DeleteManualControlRequest>) {
                    batch.manual_controls.push_back({value.id, std::nullopt});
                } else if constexpr (std::same_as<Request, SetAudioDevicesRequest>) {
                    if (batch.audio_devices) throw std::invalid_argument("project batch edits audio selection twice");
                    batch.audio_devices = ProjectAudioDeviceSelection{value.output_device_id, value.input_device_id};
                } else if constexpr (std::same_as<Request, SetProjectToolchainRequest>) {
                    if (batch.toolchain) throw std::invalid_argument("project batch edits toolchain settings twice");
                    batch.toolchain = value.settings;
                } else throw std::invalid_argument("operation is not supported in a project edit batch");
            }, nested.payload);
        }
        return parsed(ApplyProjectEditRequest{std::move(batch)});
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
        auto const has_port = params.contains("inputName") || params.contains("outputName");
        auto id = std::optional<ManualControlId>{};
        if ((node != params.end()) != has_port) {
            throw std::runtime_error(
                "manual control hover requires a node and one inputName or outputName, or neither");
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
    if (method == "manualControls.end") {
        return parsed(EndManualControlInteractionRequest{
            .interaction = parse_manual_interaction_id(params),
            .value = parse_manual_control_value(params),
        });
    }
    if (method == "observations.describe" || method == "observations.reconcile") {
        ObservationProtocolRequest request;
        request.kind = method == "observations.describe" ? ObservationProtocolRequest::Kind::describe
            : ObservationProtocolRequest::Kind::reconcile;
        if (request.kind == ObservationProtocolRequest::Kind::describe) {
            auto const& nodes = params.at("nodes");
            if (!nodes.is_array() || nodes.size() > maximum_observation_interests) throw std::invalid_argument("invalid observation nodes");
            for (auto const& node : nodes)
                request.nodes.push_back(parse_node_address_json(node, ProjectConnectionJsonNames::socket_rpc));
        } else {
            request.scope_id = parse_string_param(params, "scopeId");
            auto const& interests = params.at("interests");
            if (!interests.is_array() || interests.size() > maximum_observation_interests) throw std::invalid_argument("invalid observation interests");
            auto parse_interest = [&](Json const& item) {
                auto const& source = item.at("source");
                auto const kind = source.at("kind").get<std::string>();
                if (kind != "output" && kind != "input" && kind != "stateField")
                    throw std::invalid_argument("invalid observation source kind");
                ObservationInterest interest{
                    .id = parse_string_param(item, "id"),
                    .source = {.node = parse_node_address_json(source.at("node"), ProjectConnectionJsonNames::socket_rpc),
                        .kind = kind == "output" ? ObservationSourceKind::output
                            : kind == "input" ? ObservationSourceKind::input : ObservationSourceKind::state_field,
                        .member = parse_string_param(source, "member")},
                    .operation = parse_string_param(item, "operation"),
                };
                if (source.contains("portKind")) {
                    auto const selected = source.at("portKind").get<std::string>();
                    if (selected != "sample" && selected != "event") throw std::invalid_argument("invalid observation port kind");
                    interest.source.port_kind = selected == "event" ? PortKind::event : PortKind::sample;
                }
                if (source.contains("channel")) {
                    if (!source["channel"].is_number_unsigned()) throw std::invalid_argument("observation channel must be unsigned");
                    interest.source.channel = source["channel"].get<std::size_t>();
                }
                if (item.contains("window")) {
                    auto const& window = item.at("window");
                    auto const begin_text = parse_string_param(window, "begin");
                    auto const length_text = parse_string_param(window, "length");
                    std::int64_t begin = 0;
                    std::uint64_t length = 0;
                    auto parsed_begin = std::from_chars(begin_text.data(), begin_text.data() + begin_text.size(), begin);
                    auto parsed_length = std::from_chars(length_text.data(), length_text.data() + length_text.size(), length);
                    if (parsed_begin.ec != std::errc{} || parsed_begin.ptr != begin_text.data() + begin_text.size()
                        || parsed_length.ec != std::errc{} || parsed_length.ptr != length_text.data() + length_text.size()
                        || !length || length > static_cast<std::uint64_t>(INT64_MAX)
                        )
                        throw std::invalid_argument("invalid observation window range");
                    bool const event = interest.source.port_kind == PortKind::event;
                    auto const& count = window.at(event ? "maxMarks" : "points");
                    if (!count.is_number_unsigned()) throw std::invalid_argument("invalid observation window budget");
                    auto const points = count.get<std::uint64_t>();
                    if (!points || points > maximum_observation_window_points || interest.source.kind == ObservationSourceKind::state_field)
                        throw std::invalid_argument("invalid observation window points or source");
                    interest.window = ObservationWindow{GlobalIndex::from_signed(begin), length, static_cast<std::size_t>(points)};
                    if (event) {
                        auto const& depth = window.at("maxDepth");
                        if (interest.operation != "beat-grid" || interest.source.kind != ObservationSourceKind::input
                            || interest.source.channel || !depth.is_number_unsigned() || depth.get<std::uint64_t>() > 255)
                            throw std::invalid_argument("invalid beat-grid observation");
                        interest.window->max_depth = depth.get<std::uint8_t>();
                    }
                }
                if (interest.source.port_kind == PortKind::event && !interest.window)
                    throw std::invalid_argument("event observation requires a window");
                if (item.contains("prefetch")) {
                    if (!interest.window || !item.at("prefetch").is_boolean())
                        throw std::invalid_argument("observation prefetch requires a window and a boolean priority flag");
                    interest.prefetch = item.at("prefetch").get<bool>();
                }
                return interest;
            };
            for (auto const& item : interests) request.interests.push_back(parse_interest(item));
            if (auto const prediction = params.find("predictedInterests"); prediction != params.end()) {
                if (!prediction->is_array() || prediction->size() > maximum_live_observation_probes)
                    throw std::invalid_argument("invalid predicted observation interests");
                for (auto const& item : *prediction) {
                    auto interest = parse_interest(item);
                    if (interest.window) throw std::invalid_argument("observation predictions require live probes");
                    request.predicted_interests.push_back(std::move(interest));
                }
            }
        }
        if (request.kind == ObservationProtocolRequest::Kind::describe)
            return parsed(DescribeObservationsRequest{std::move(request)});
        return parsed(ReconcileObservationsRequest{std::move(request)});
    }
    if (method == "nodeMessages.getProtocol") {
        return parsed(GetNodeEventProtocolRequest{});
    }
    if (method == "nodeMessages.beginGesture" || method == "nodeMessages.endGesture") {
        NodeMessageGestureRequest gesture{
            .id = {.interaction_id = parse_string_param(params, "interactionId")},
            .target = parse_node_address_json(params.at("target"), ProjectConnectionJsonNames::socket_rpc),
            .begin = method == "nodeMessages.beginGesture",
        };
        if (gesture.id.interaction_id.empty()) throw std::invalid_argument("node-message gesture requires an interaction id");
        if (gesture.begin) return parsed(BeginNodeMessageGestureRequest{std::move(gesture)});
        return parsed(EndNodeMessageGestureRequest{std::move(gesture)});
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
                .gesture = params.contains("interactionId")
                    ? std::optional<NodeMessageGestureId>(NodeMessageGestureId{.interaction_id = parse_string_param(params, "interactionId")})
                    : std::nullopt,
            });
    }
    if (method == "audioDevices.get") {
        return parsed(GetAudioDevicesRequest{});
    }
    if (method == "playback.get") return parsed(GetPlaybackRequest{});
    if (method == "playback.toggle") return parsed(TogglePlaybackRequest{});
    if (method == "playback.setRunning") {
        if (!params.contains("running") || !params.at("running").is_boolean())
            throw std::invalid_argument("playback.setRunning requires boolean param 'running'");
        return parsed(SetPlaybackRunningRequest{params.at("running").get<bool>()});
    }
    if (method == "playback.seek" || method == "playback.drag") {
        auto const text = parse_string_param(params, "position");
        std::int64_t position = 0;
        auto const converted = std::from_chars(text.data(), text.data() + text.size(), position);
        if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size())
            throw std::invalid_argument(method + " requires a signed decimal sample index string");
        auto const index = GlobalIndex::from_signed(position);
        if (method == "playback.seek") return parsed(SeekPlaybackRequest{index});

        auto const phase_name = parse_string_param(params, "phase");
        ProjectPlaybackDragPhase phase;
        if (phase_name == "begin") phase = ProjectPlaybackDragPhase::begin;
        else if (phase_name == "move") phase = ProjectPlaybackDragPhase::move;
        else if (phase_name == "end") phase = ProjectPlaybackDragPhase::end;
        else throw std::invalid_argument("playback.drag requires 'begin', 'move', or 'end' for 'phase'");

        auto const drag_id = parse_string_param(params, "dragId");
        std::uint64_t id = 0;
        auto const id_result = std::from_chars(drag_id.data(), drag_id.data() + drag_id.size(), id);
        if (id_result.ec != std::errc{} || id_result.ptr != drag_id.data() + drag_id.size())
            throw std::invalid_argument("playback.drag requires an unsigned decimal 'dragId' string");
        return parsed(DragPlaybackRequest{index, {phase, id}});
    }
    if (method == "audioDevices.set") {
        return parsed(SetAudioDevicesRequest{
            .output_device_id = parse_optional_nullable_string_param(params, "outputDeviceId"),
            .input_device_id = parse_optional_nullable_string_param(params, "inputDeviceId")});
    }
    if (method == "project.save") {
        return parsed(SaveProjectRequest{.autosave_only = params.value("autosaveOnly", false)});
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
