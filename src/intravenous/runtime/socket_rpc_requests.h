#pragma once

#include <intravenous/runtime/lane_view_service.h>
#include <intravenous/runtime/runtime_project_api_types.h>

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
    std::optional<std::string> instance_id{};
};

struct GraphQueryActiveRegionsRequest { std::filesystem::path file_path{}; };
struct GetVirtualNodeRequest { std::string node_id{}; };
struct GetVirtualNodesRequest { std::vector<std::string> node_ids{}; };

struct CreateIvModuleInstanceRequest {
    std::string module_id{};
    std::optional<std::string> display_name{};
};
struct GetIvPackageDefinitionsRequest {};
struct CreateIvPackageRequest { std::string name{}; };
struct GetIvModuleInstancesRequest {
    std::optional<std::filesystem::path> source_file_path {};
};
struct DeleteIvModuleInstanceRequest { std::string instance_id{}; };
struct UpdateIvModuleInstance {
    std::string instance_id{};
    std::optional<std::string> display_name{};
};
struct UpdateIvModuleInstancesRequest {
    std::vector<UpdateIvModuleInstance> updates{};
};

struct GetAudioDevicesRequest {};
struct SetAudioDevicesRequest {
    std::optional<std::string> output_device_id {};
    std::optional<std::string> input_device_id {};
};

// Retained but intentionally disconnected while the canonical project graph
// replaces the deleted timeline-lane execution source.
struct OpenLaneViewRpcRequest { LaneViewRequest request{}; };
struct UpdateLaneViewRpcRequest { LaneViewRequest request{}; };
struct GetLaneQuerySchemaRequest {};
struct CompleteLaneQueryRequest {
    std::string source {};
    size_t cursor_offset = 0;
    std::optional<std::uint64_t> schema_revision {};
};

struct SaveProjectRequest {};
struct EnableProjectAutosaveRequest {};
struct DisableProjectAutosaveRequest {};
struct ServerShutdownRequest {};
struct UnsupportedSocketRpcRequest { std::string method{}; };

using SocketRpcRequestPayload = std::variant<
    GraphQueryBySpansRequest,
    GraphQueryActiveRegionsRequest,
    GetVirtualNodeRequest,
    GetVirtualNodesRequest,
    CreateIvModuleInstanceRequest,
    GetIvPackageDefinitionsRequest,
    CreateIvPackageRequest,
    GetIvModuleInstancesRequest,
    DeleteIvModuleInstanceRequest,
    UpdateIvModuleInstancesRequest,
    GetAudioDevicesRequest,
    SetAudioDevicesRequest,
    OpenLaneViewRpcRequest,
    UpdateLaneViewRpcRequest,
    GetLaneQuerySchemaRequest,
    CompleteLaneQueryRequest,
    SaveProjectRequest,
    EnableProjectAutosaveRequest,
    DisableProjectAutosaveRequest,
    ServerShutdownRequest,
    UnsupportedSocketRpcRequest,
    std::string>;

struct ParsedSocketRpcRequest {
    int request_id = 0;
    SocketRpcRequestPayload payload{};
};

ParsedSocketRpcRequest parse_socket_rpc_request(std::string_view line);
} // namespace iv
