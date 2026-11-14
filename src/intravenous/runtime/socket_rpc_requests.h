#pragma once

#include <intravenous/runtime/project_edit.h>

#include <intravenous/runtime/runtime_project_api_types.h>
#include <intravenous/runtime/project_connection_types.h>
#include <intravenous/node/manual_control.h>

#include <filesystem>
#include <array>
#include <functional>
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
struct ApplyProjectEditRequest { ProjectEditBatch batch{}; };
struct UndoProjectEditRequest { std::function<void()> authorize_audio_control{}; };
struct RedoProjectEditRequest { std::function<void()> authorize_audio_control{}; };

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
// One transient gesture value. A client sends it as an id-less JSON-RPC
// notification; the server handles it on that client's reader thread, outside
// the ordered command queue, and sends no reply. It changes no durable state and
// takes no session revision. The results return on this record so the server can
// relay the accepted value to the other clients.
struct LiveManualControlUpdateRequest {
    ManualControlInteractionId interaction{};
    Sample value{};
    bool applied = false;
    ManualControlId control{};
};
// Identifies one gesture's relayed live values among all clients' frames.
inline std::string live_gesture_key(ManualControlInteractionId const& interaction)
{
    return interaction.client_id + '\n' + interaction.interaction_id;
}
struct EndManualControlInteractionRequest {
    ManualControlInteractionId interaction{};
    Sample value{};
};
// One closed client connection. The client's open gestures end at the last
// values the server received from it; project_state_changed reports whether that
// committed durable state, so the server publishes one revision for it.
struct SocketRpcClientDisconnectedRequest {
    std::string client_id{};
    bool project_state_changed = false;
};

struct DispatchNodeMessageRequest {
    NodeAddress target{};
    std::string event_type_nominal_id{};
    std::string event_type_definition_fingerprint{};
    std::shared_ptr<nlohmann::ordered_json const> payload{};
};
struct GetNodeEventProtocolRequest {};

struct GetAudioDevicesRequest {};
// Temporary output diagnostics. This does not mutate the project revision.
struct CaptureAudioOutputRequest {
    std::string action{};
    std::string token{};
    bool handled = false;
    bool recording = false;
    std::string path{};
    std::size_t sample_rate = 0;
    std::size_t channels = 0;
    std::size_t sample_count = 0;
    std::uint64_t server_time_ms = 0;
    std::uint64_t server_steady_us = 0;
    std::vector<std::array<std::uint64_t, 3>> blocks{};
};
struct SetProjectToolchainRequest { ProjectToolchainSettings settings{}; };
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
    ApplyProjectEditRequest,
    UndoProjectEditRequest,
    RedoProjectEditRequest,
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
    EndManualControlInteractionRequest,
    GetNodeEventProtocolRequest,
    DispatchNodeMessageRequest,
    GetAudioDevicesRequest,
    CaptureAudioOutputRequest,
    SetProjectToolchainRequest,
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

// Returns the update carried by an id-less `manualControls.update` notification,
// or nothing for every other message. Throws when such a notification is malformed.
std::optional<LiveManualControlUpdateRequest>
parse_live_manual_control_update(std::string_view line);
} // namespace iv
