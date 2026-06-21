#pragma once

#include <intravenous/lane_node/channels.h>
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

    struct SetTimelineCompiledSampleCacheChunkSizeMultiplierRequest {
        size_t compiled_sample_cache_chunk_size_multiplier = 0;
    };

    struct SetTimelineLaneSampleChannelTypeRequest {
        std::uint64_t lane_id = 0;
        ChannelTypeId sample_channel_type = ChannelTypeId::stereo;
    };

    struct GetAudioDevicesRequest {};

    struct SetAudioDevicesRequest {
        std::optional<std::string> output_device_id {};
        std::optional<std::string> input_device_id {};
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

    struct SetSampleInputStateRequest {
        std::string node_id{};
        size_t input_ordinal = 0;
        std::optional<size_t> member_ordinal{};
        std::string state{};
    };

    struct SetEventInputStateRequest {
        std::string node_id{};
        size_t input_ordinal = 0;
        std::optional<size_t> member_ordinal{};
        std::string state{};
    };

    struct SetSampleOutputStateRequest {
        std::string node_id{};
        size_t output_ordinal = 0;
        std::optional<size_t> member_ordinal{};
        std::string state{};
    };

    struct SetEventOutputStateRequest {
        std::string node_id{};
        size_t output_ordinal = 0;
        std::optional<size_t> member_ordinal{};
        std::string state{};
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
        SetTimelineCompiledSampleCacheChunkSizeMultiplierRequest,
        SetTimelineLaneSampleChannelTypeRequest,
        GetAudioDevicesRequest,
        SetAudioDevicesRequest,
        OpenLaneViewRpcRequest,
        UpdateLaneViewRpcRequest,
        SetSampleInputValueRequest,
        SetSampleInputStateRequest,
        SetEventInputStateRequest,
        SetSampleOutputStateRequest,
        SetEventOutputStateRequest,
        ServerShutdownRequest,
        std::string>;

    struct ParsedSocketRpcRequest {
        int request_id = 0;
        SocketRpcRequestPayload payload{};
    };

    ParsedSocketRpcRequest parse_socket_rpc_request(std::string_view line);
} // namespace iv
