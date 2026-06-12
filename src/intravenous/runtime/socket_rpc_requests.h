#pragma once

#include <intravenous/runtime/lane_view_service.h>
#include <intravenous/runtime/runtime_project_api_types.h>
#include <intravenous/sample.h>

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

    struct CreateIvModuleInstanceRequest {
        std::filesystem::path module_root{};
    };

    struct DeleteIvModuleInstanceRequest {
        std::string instance_id{};
    };

    struct SetIvModuleInstanceDefaultSilenceTtlSamplesRequest {
        std::string instance_id{};
        size_t default_silence_ttl_samples = 0;
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
        CreateIvModuleInstanceRequest,
        DeleteIvModuleInstanceRequest,
        SetIvModuleInstanceDefaultSilenceTtlSamplesRequest,
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
