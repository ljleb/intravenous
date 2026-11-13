#pragma once

#include <intravenous/runtime/runtime_project_api_types.h>
#include <intravenous/runtime/project_connection_types.h>
#include <intravenous/node/manual_control.h>

#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json_fwd.hpp>

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
// The graph editor's project root scope; see ProjectGraphScope.
struct GetGraphScopeRequest {};

struct CreateNodeInstanceRequest {
    std::optional<std::string> instance_id{};
    std::string definition_id{};
    std::optional<std::string> display_name{};
    std::string configuration_source{};
};
struct GetIvPackageDefinitionsRequest {};
struct CreateIvPackageRequest { std::string name{}; };
struct GetNodeInstancesRequest {
    std::optional<std::filesystem::path> source_file_path {};
};
struct DeleteNodeInstanceRequest { std::string instance_id{}; };
struct UpdateNodeInstance {
    std::string instance_id{};
    std::optional<std::string> display_name{};
    std::optional<std::string> configuration_source{};
};
struct UpdateNodeInstancesRequest {
    std::vector<UpdateNodeInstance> updates{};
};
struct UpsertGraphConnectionRequest { ProjectConnection connection{}; };
struct DeleteGraphConnectionRequest { std::string connection_id{}; };
struct UpsertManualControlRequest { ProjectManualControl control{}; };
struct DeleteManualControlRequest { ManualControlId id{}; };
struct SetManualControlHoverRequest {
    std::string client_id{};
    std::optional<ManualControlId> id{};
};
struct BeginManualControlInteractionRequest {
    ManualControlInteractionId interaction{};
    ManualControlId id{};
    Sample value{};
};
struct UpdateManualControlInteractionRequest {
    ManualControlInteractionId interaction{};
    Sample value{};
};
struct EndManualControlInteractionRequest {
    ManualControlInteractionId interaction{};
    Sample value{};
};

struct DispatchNodeMessageRequest {
    NodeAddress target{};
    std::string event_type_nominal_id{};
    std::string event_type_definition_fingerprint{};
    std::shared_ptr<nlohmann::ordered_json const> payload{};
};
struct GetNodeEventProtocolRequest {};

struct GetAudioDevicesRequest {};
struct SetAudioDevicesRequest {
    std::optional<std::string> output_device_id {};
    std::optional<std::string> input_device_id {};
};

struct SaveProjectRequest {};
struct EnableProjectAutosaveRequest {};
struct DisableProjectAutosaveRequest {};
struct SessionHelloRequest {
    std::string client_id{};
    std::string local_capability{};
};
struct SessionSubscribeRequest {
    std::uint64_t after_revision = 0;
    std::vector<std::string> topics{};
};
struct SessionSnapshotRequest {};
struct ServerShutdownRequest {};
struct UnsupportedSocketRpcRequest { std::string method{}; };

using SocketRpcRequestPayload = std::variant<
    GraphQueryBySpansRequest,
    GraphQueryActiveRegionsRequest,
    GetVirtualNodeRequest,
    GetVirtualNodesRequest,
    GetGraphScopeRequest,
    CreateNodeInstanceRequest,
    GetIvPackageDefinitionsRequest,
    CreateIvPackageRequest,
    GetNodeInstancesRequest,
    DeleteNodeInstanceRequest,
    UpdateNodeInstancesRequest,
    UpsertGraphConnectionRequest,
    DeleteGraphConnectionRequest,
    UpsertManualControlRequest,
    DeleteManualControlRequest,
    SetManualControlHoverRequest,
    BeginManualControlInteractionRequest,
    UpdateManualControlInteractionRequest,
    EndManualControlInteractionRequest,
    GetNodeEventProtocolRequest,
    DispatchNodeMessageRequest,
    GetAudioDevicesRequest,
    SetAudioDevicesRequest,
    SaveProjectRequest,
    EnableProjectAutosaveRequest,
    DisableProjectAutosaveRequest,
    SessionHelloRequest,
    SessionSubscribeRequest,
    SessionSnapshotRequest,
    ServerShutdownRequest,
    UnsupportedSocketRpcRequest>;

struct ParsedSocketRpcRequest {
    int request_id = 0;
    std::string method{};
    std::optional<std::uint64_t> expected_revision{};
    SocketRpcRequestPayload payload{};
};

ParsedSocketRpcRequest parse_socket_rpc_request(std::string_view line);
} // namespace iv
