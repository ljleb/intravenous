#include <intravenous/runtime/socket_rpc_requests.h>

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

uint64_t parse_uint64_param(Json const &params, std::string const &key) {
    auto const value_it = params.find(key);
    if (value_it == params.end() || !value_it->is_number_integer()) {
        throw std::runtime_error("JSON-RPC request is missing integer param '" + key + "'");
    }
    auto const value = value_it->get<int64_t>();
    if (value < 0) {
        throw std::runtime_error("JSON-RPC request param '" + key + "' must be non-negative");
    }
    return static_cast<uint64_t>(value);
}

std::optional<uint64_t> parse_optional_uint64_param(Json const &params, std::string const &key) {
    auto const value_it = params.find(key);
    if (value_it == params.end() || value_it->is_null()) {
        return std::nullopt;
    }
    if (!value_it->is_number_integer()) {
        throw std::runtime_error("JSON-RPC request param '" + key + "' must be a non-negative integer");
    }
    auto const value = value_it->get<int64_t>();
    if (value < 0) {
        throw std::runtime_error("JSON-RPC request param '" + key + "' must be non-negative");
    }
    return static_cast<uint64_t>(value);
}

size_t parse_optional_size_param(Json const &params, std::string const &key, size_t fallback) {
    auto const value_it = params.find(key);
    if (value_it == params.end() || value_it->is_null()) {
        return fallback;
    }
    if (!value_it->is_number_integer()) {
        throw std::runtime_error("JSON-RPC request param '" + key + "' must be a non-negative integer");
    }
    auto const value = value_it->get<int64_t>();
    if (value < 0) {
        throw std::runtime_error("JSON-RPC request param '" + key + "' must be non-negative");
    }
    return static_cast<size_t>(value);
}

double parse_number_param(Json const &params, std::string const &key) {
    auto const value_it = params.find(key);
    if (value_it == params.end() || !value_it->is_number()) {
        throw std::runtime_error("JSON-RPC request is missing numeric param '" + key + "'");
    }
    return value_it->get<double>();
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

LaneQueryFilter parse_lane_query_filter(Json const &params) {
    auto const filter_it = params.find("filter");
    if (filter_it == params.end() || !filter_it->is_object()) {
        throw std::runtime_error("lane requests require a filter object");
    }
    auto const query_it = filter_it->find("query");
    if (query_it != filter_it->end()) {
        if (!query_it->is_string()) {
            throw std::runtime_error("lane filter.query must be a string");
        }
        return LaneQueryFilter{.source = query_it->get<std::string>()};
    }
    auto const kind_it = filter_it->find("kind");
    if (kind_it == filter_it->end() || !kind_it->is_string()) {
        throw std::runtime_error("lane filter must provide string query or kind");
    }
    auto const kind = kind_it->get<std::string>();
    if (kind == "graphInputs") {
        return LaneQueryFilter{.source = "dsp_graph.graph_input"};
    }
    return LaneQueryFilter{.source = kind};
}

LaneQuery parse_lane_query(Json const &params) {
    return LaneQuery{.filter = parse_lane_query_filter(params)};
}

LaneViewRequest parse_lane_view_request(Json const &params) {
    return LaneViewRequest{
        .view_id = parse_string_param(params, "viewId"),
        .query = parse_lane_query(params),
        .start_index = parse_optional_size_param(params, "startIndex", 0),
        .visible_lane_count = parse_optional_size_param(params, "visibleLaneCount", 0),
    };
}
} // namespace

ParsedSocketRpcRequest parse_socket_rpc_request(std::string_view line) {
    auto const request = parse_request_json(line);
    auto const request_id = parse_request_id(request);
    auto const method = parse_request_method(request);
    auto const &params = parse_request_params(request);

    if (method == "graph.queryBySpans") {
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .payload = GraphQueryBySpansRequest{
                .file_path = parse_string_param(params, "filePath"),
                .ranges = parse_ranges(params),
                .match_mode = parse_match_mode(params),
            },
        };
    }
    if (method == "graph.queryActiveRegions") {
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .payload = GraphQueryActiveRegionsRequest{
                .file_path = parse_string_param(params, "filePath"),
            },
        };
    }
    if (method == "graph.getLogicalNode") {
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .payload = GetLogicalNodeRequest{
                .node_id = parse_string_param(params, "nodeId"),
            },
        };
    }
    if (method == "graph.getLogicalNodes") {
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .payload = GetLogicalNodesRequest{
                .node_ids = parse_string_array_param(params, "nodeIds"),
            },
        };
    }
    if (method == "ivModuleInstances.create") {
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .payload = CreateIvModuleInstanceRequest{
                .module_root = parse_string_param(params, "moduleRoot"),
            },
        };
    }
    if (method == "ivModuleInstances.delete") {
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .payload = DeleteIvModuleInstanceRequest{
                .instance_id = parse_string_param(params, "instanceId"),
            },
        };
    }
    if (method == "ivModuleInstances.setDefaultSilenceTtlSamples") {
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .payload = SetIvModuleInstanceDefaultSilenceTtlSamplesRequest{
                .instance_id = parse_string_param(params, "instanceId"),
                .default_silence_ttl_samples =
                    static_cast<size_t>(parse_uint64_param(
                        params,
                        "defaultSilenceTtlSamples")),
            },
        };
    }
    if (method == "timeline.setCompiledSampleCacheChunkSizeMultiplier") {
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .payload = SetTimelineCompiledSampleCacheChunkSizeMultiplierRequest{
                .compiled_sample_cache_chunk_size_multiplier =
                    static_cast<size_t>(parse_uint64_param(
                        params,
                        "compiledSampleCacheChunkSizeMultiplier")),
            },
        };
    }
    if (method == "timeline.openLaneView" || method == "timeline.updateLaneView") {
        auto const request_payload = parse_lane_view_request(params);
        if (method == "timeline.openLaneView") {
            return ParsedSocketRpcRequest{
                .request_id = request_id,
                .payload = OpenLaneViewRpcRequest{
                    .request = std::move(request_payload),
                },
            };
        }
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .payload = UpdateLaneViewRpcRequest{
                .request = std::move(request_payload),
            },
        };
    }
    if (method == "timeline.closeLaneView") {
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .payload = parse_string_param(params, "viewId"),
        };
    }
    if (method == "graph.setSampleInputValue") {
        auto const member_ordinal = parse_optional_uint64_param(params, "memberOrdinal");
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .payload = SetSampleInputValueRequest{
                .node_id = parse_string_param(params, "nodeId"),
                .input_ordinal = static_cast<size_t>(parse_uint64_param(params, "inputOrdinal")),
                .value = static_cast<Sample>(parse_number_param(params, "value")),
                .member_ordinal = member_ordinal.has_value()
                    ? std::optional<size_t>(static_cast<size_t>(*member_ordinal))
                    : std::nullopt,
                },
        };
    }
    if (method == "graph.setSampleInputState") {
        auto const member_ordinal = parse_optional_uint64_param(params, "memberOrdinal");
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .payload = SetSampleInputStateRequest{
                .node_id = parse_string_param(params, "nodeId"),
                .input_ordinal = static_cast<size_t>(parse_uint64_param(params, "inputOrdinal")),
                .member_ordinal = member_ordinal.has_value()
                    ? std::optional<size_t>(static_cast<size_t>(*member_ordinal))
                    : std::nullopt,
                .state = parse_string_param(params, "state"),
            },
        };
    }
    if (method == "graph.setEventInputState") {
        auto const member_ordinal = parse_optional_uint64_param(params, "memberOrdinal");
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .payload = SetEventInputStateRequest{
                .node_id = parse_string_param(params, "nodeId"),
                .input_ordinal = static_cast<size_t>(parse_uint64_param(params, "inputOrdinal")),
                .member_ordinal = member_ordinal.has_value()
                    ? std::optional<size_t>(static_cast<size_t>(*member_ordinal))
                    : std::nullopt,
                .state = parse_string_param(params, "state"),
            },
        };
    }
    if (method == "graph.setSampleOutputState") {
        auto const member_ordinal = parse_optional_uint64_param(params, "memberOrdinal");
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .payload = SetSampleOutputStateRequest{
                .node_id = parse_string_param(params, "nodeId"),
                .output_ordinal = static_cast<size_t>(parse_uint64_param(params, "outputOrdinal")),
                .member_ordinal = member_ordinal.has_value()
                    ? std::optional<size_t>(static_cast<size_t>(*member_ordinal))
                    : std::nullopt,
                .state = parse_string_param(params, "state"),
            },
        };
    }
    if (method == "graph.setEventOutputState") {
        auto const member_ordinal = parse_optional_uint64_param(params, "memberOrdinal");
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .payload = SetEventOutputStateRequest{
                .node_id = parse_string_param(params, "nodeId"),
                .output_ordinal = static_cast<size_t>(parse_uint64_param(params, "outputOrdinal")),
                .member_ordinal = member_ordinal.has_value()
                    ? std::optional<size_t>(static_cast<size_t>(*member_ordinal))
                    : std::nullopt,
                .state = parse_string_param(params, "state"),
            },
        };
    }
    if (method == "server.shutdown") {
        return ParsedSocketRpcRequest{
            .request_id = request_id,
            .payload = ServerShutdownRequest{},
        };
    }

    throw std::runtime_error("unsupported JSON-RPC method: " + method);
}
} // namespace iv
