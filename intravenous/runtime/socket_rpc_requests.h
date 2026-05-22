#pragma once

#include "runtime/lane_view_service.h"
#include "runtime/runtime_project_api_types.h"
#include "sample.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace iv {
    struct GraphQueryBySpansRequest {
        std::filesystem::path file_path{};
        std::vector<SourceRange> ranges{};
        SourceRangeMatchMode match_mode = SourceRangeMatchMode::intersection;
    };

    struct GraphQueryActiveRegionsRequest {
        std::filesystem::path file_path{};
    };

    struct GetLogicalNodeRequest {
        std::string node_id{};
    };

    struct GetLogicalNodesRequest {
        std::vector<std::string> node_ids{};
    };

    struct OpenLaneViewRpcRequest {
        LaneViewRequest request{};
    };

    struct UpdateLaneViewRpcRequest {
        LaneViewRequest request{};
    };

    struct SetSampleInputValueRequest {
        std::string node_id{};
        size_t input_ordinal = 0;
        Sample value = 0.0f;
        std::optional<size_t> member_ordinal{};
    };

    struct ClearSampleInputValueOverrideRequest {
        std::string node_id{};
        size_t member_ordinal = 0;
        size_t input_ordinal = 0;
    };

    struct ServerShutdownRequest {};

    using SocketRpcRequestPayload = std::variant<
        GraphQueryBySpansRequest,
        GraphQueryActiveRegionsRequest,
        GetLogicalNodeRequest,
        GetLogicalNodesRequest,
        OpenLaneViewRpcRequest,
        UpdateLaneViewRpcRequest,
        SetSampleInputValueRequest,
        ClearSampleInputValueOverrideRequest,
        ServerShutdownRequest,
        std::string>;

    struct ParsedSocketRpcRequest {
        int request_id = 0;
        SocketRpcRequestPayload payload{};
    };

    ParsedSocketRpcRequest parse_socket_rpc_request(std::string_view line);
} // namespace iv
