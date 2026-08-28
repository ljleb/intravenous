#pragma once

#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/lane_views.h>
#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/startup_config.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_execution.h>

#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace iv {
class SocketRpcAckResponseBuilder;
class SocketRpcCreateIvModuleInstanceResultBuilder;
class SocketRpcLaneTypesResultBuilder;
struct SaveProjectRequest;
struct CreateIvModuleInstanceRequest;
struct DeleteIvModuleInstanceRequest;
struct UpdateIvModuleInstancesRequest;
struct SetTimelineCompiledSampleCacheChunkSizeMultiplierRequest;
struct SetTimelineLaneSampleChannelTypeRequest;
struct SetTimelineLaneUiStateRequest;
struct ConnectTimelineLanesRequest;
struct DisconnectTimelineLanesRequest;
struct GetTimelineLaneTypesRequest;
struct CreateTimelineLaneRequest;
struct DeleteTimelineLaneRequest;
struct DuplicateTimelineLaneRequest;

class ProjectPersistence {
    std::filesystem::path workspace_root_;
    StartupConfigState startup_;
    mutable std::mutex save_mutex_;

    [[nodiscard]] std::filesystem::path project_file_path() const;
    [[nodiscard]] std::vector<ProjectCommand> read_commands() const;
    void apply_command(ProjectCommand const &command);
    void emit_message(std::string level, std::string message) const;

public:
    ProjectPersistence(
        std::filesystem::path workspace_root,
        StartupConfigState startup);

    void load();
    void save() const;
    void handle_socket_rpc_save_project(
        SaveProjectRequest const &request,
        SocketRpcAckResponseBuilder &builder) const;
    void handle_socket_rpc_create_iv_module_instance(
        CreateIvModuleInstanceRequest const &request,
        SocketRpcCreateIvModuleInstanceResultBuilder &builder) const;
    void handle_socket_rpc_delete_iv_module_instance(
        DeleteIvModuleInstanceRequest const &request,
        SocketRpcAckResponseBuilder &builder) const;
    void handle_socket_rpc_update_iv_module_instances(
        UpdateIvModuleInstancesRequest const &request,
        SocketRpcAckResponseBuilder &builder) const;
    void handle_socket_rpc_set_timeline_compiled_sample_cache_chunk_size_multiplier(
        SetTimelineCompiledSampleCacheChunkSizeMultiplierRequest const &request,
        SocketRpcAckResponseBuilder &builder) const;
    void handle_socket_rpc_set_timeline_lane_sample_channel_type(
        SetTimelineLaneSampleChannelTypeRequest const &request,
        SocketRpcAckResponseBuilder &builder) const;
    void handle_socket_rpc_set_timeline_lane_ui_state(
        SetTimelineLaneUiStateRequest const &request,
        SocketRpcAckResponseBuilder &builder) const;
    void handle_socket_rpc_connect_timeline_lanes(
        ConnectTimelineLanesRequest const &request,
        SocketRpcAckResponseBuilder &builder) const;
    void handle_socket_rpc_disconnect_timeline_lanes(
        DisconnectTimelineLanesRequest const &request,
        SocketRpcAckResponseBuilder &builder) const;
    void handle_socket_rpc_get_timeline_lane_types(
        GetTimelineLaneTypesRequest const &request,
        SocketRpcLaneTypesResultBuilder &builder) const;
    void handle_socket_rpc_create_timeline_lane(
        CreateTimelineLaneRequest const &request,
        SocketRpcAckResponseBuilder &builder) const;
    void handle_socket_rpc_delete_timeline_lane(
        DeleteTimelineLaneRequest const &request,
        SocketRpcAckResponseBuilder &builder) const;
    void handle_socket_rpc_duplicate_timeline_lane(
        DuplicateTimelineLaneRequest const &request,
        SocketRpcAckResponseBuilder &builder) const;
    void report_autosave_failure(std::string message) const;
};
} // namespace iv
