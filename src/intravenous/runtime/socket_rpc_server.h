#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/linker_event.h>
#include <intravenous/runtime/lane_view_service.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/lanes_visualization_api_types.h>
#include <intravenous/runtime/runtime_project_api_types.h>
#include <intravenous/runtime/socket_rpc_requests.h>
#include <intravenous/runtime/socket_rpc_response_builders.h>
#include <intravenous/sample.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace iv {
    using SocketRpcGraphQueryResult = ProjectQueryResult;
    using SocketRpcRegionQueryResult = ProjectRegionQueryResult;
    using SocketRpcServerMessage = ProjectMessageNotification;
    using SocketRpcServerStatus = ProjectStatusNotification;

    using SocketRpcGraphQueryBySpansEvent =
        void (*)(GraphQueryBySpansRequest const &, SocketRpcGraphQueryResultBuilder &);
    using SocketRpcGraphQueryActiveRegionsEvent =
        void (*)(GraphQueryActiveRegionsRequest const &, SocketRpcRegionQueryResultBuilder &);
    using SocketRpcGetLogicalNodeEvent =
        void (*)(GetLogicalNodeRequest const &, SocketRpcLogicalNodeResultBuilder &);
    using SocketRpcGetLogicalNodesEvent =
        void (*)(GetLogicalNodesRequest const &, SocketRpcLogicalNodesResultBuilder &);
    using SocketRpcCreateIvModuleInstanceEvent =
        void (*)(CreateIvModuleInstanceRequest const &, SocketRpcCreateIvModuleInstanceResultBuilder &);
    using SocketRpcGetIvModuleSourcesEvent =
        void (*)(GetIvModuleSourcesRequest const &, SocketRpcIvModuleSourcesResultBuilder &);
    using SocketRpcCreateIvModuleSourceEvent =
        void (*)(CreateIvModuleSourceRequest const &, SocketRpcIvModuleSourceResultBuilder &);
    using SocketRpcGetIvModuleInstancesEvent =
        void (*)(GetIvModuleInstancesRequest const &, SocketRpcIvModuleInstancesResultBuilder &);
    using SocketRpcDeleteIvModuleInstanceEvent =
        void (*)(DeleteIvModuleInstanceRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcUpdateIvModuleInstancesEvent =
        void (*)(UpdateIvModuleInstancesRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcSetTimelineCompiledSampleCacheChunkSizeMultiplierEvent =
        void (*)(SetTimelineCompiledSampleCacheChunkSizeMultiplierRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcSetTimelineLaneSampleChannelTypeEvent =
        void (*)(SetTimelineLaneSampleChannelTypeRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcGetAudioDevicesEvent =
        void (*)(GetAudioDevicesRequest const &, SocketRpcAudioDevicesResultBuilder &);
    using SocketRpcSetAudioDevicesEvent =
        void (*)(SetAudioDevicesRequest const &, SocketRpcAudioDevicesResultBuilder &);
    using SocketRpcOpenLaneViewEvent =
        void (*)(LaneViewRequest const &, SocketRpcLaneViewResultBuilder &);
    using SocketRpcUpdateLaneViewEvent =
        void (*)(LaneViewRequest const &, SocketRpcLaneViewResultBuilder &);
    using SocketRpcCloseLaneViewEvent =
        void (*)(std::string const &, SocketRpcAckResponseBuilder &);
    using SocketRpcSetSampleInputValueEvent =
        void (*)(SetSampleInputValueRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcSetSampleInputStateEvent =
        void (*)(SetSampleInputStateRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcSetEventInputStateEvent =
        void (*)(SetEventInputStateRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcSetSampleOutputStateEvent =
        void (*)(SetSampleOutputStateRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcSetEventOutputStateEvent =
        void (*)(SetEventOutputStateRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcPauseEvent =
        void (*)(PauseRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcResumeEvent =
        void (*)(ResumeRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcSaveProjectEvent =
        void (*)(SaveProjectRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcEnableProjectAutosaveEvent =
        void (*)(EnableProjectAutosaveRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcDisableProjectAutosaveEvent =
        void (*)(DisableProjectAutosaveRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcServerShutdownEvent =
        void (*)(ServerShutdownRequest const &, SocketRpcAckResponseBuilder &);

    IV_DECLARE_LINKER_EVENT(SocketRpcGraphQueryBySpansEvent, iv_socket_rpc_graph_query_by_spans_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGraphQueryActiveRegionsEvent, iv_socket_rpc_graph_query_active_regions_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGetLogicalNodeEvent, iv_socket_rpc_get_logical_node_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGetLogicalNodesEvent, iv_socket_rpc_get_logical_nodes_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcCreateIvModuleInstanceEvent, iv_socket_rpc_create_iv_module_instance_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGetIvModuleSourcesEvent, iv_socket_rpc_get_iv_module_sources_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcCreateIvModuleSourceEvent, iv_socket_rpc_create_iv_module_source_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGetIvModuleInstancesEvent, iv_socket_rpc_get_iv_module_instances_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcDeleteIvModuleInstanceEvent, iv_socket_rpc_delete_iv_module_instance_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcUpdateIvModuleInstancesEvent, iv_socket_rpc_update_iv_module_instances_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcSetTimelineCompiledSampleCacheChunkSizeMultiplierEvent, iv_socket_rpc_set_timeline_compiled_sample_cache_chunk_size_multiplier_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcSetTimelineLaneSampleChannelTypeEvent, iv_socket_rpc_set_timeline_lane_sample_channel_type_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGetAudioDevicesEvent, iv_socket_rpc_get_audio_devices_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcSetAudioDevicesEvent, iv_socket_rpc_set_audio_devices_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcOpenLaneViewEvent, iv_socket_rpc_open_lane_view_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcUpdateLaneViewEvent, iv_socket_rpc_update_lane_view_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcCloseLaneViewEvent, iv_socket_rpc_close_lane_view_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcSetSampleInputValueEvent, iv_socket_rpc_set_sample_input_value_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcSetSampleInputStateEvent, iv_socket_rpc_set_sample_input_state_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcSetEventInputStateEvent, iv_socket_rpc_set_event_input_state_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcSetSampleOutputStateEvent, iv_socket_rpc_set_sample_output_state_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcSetEventOutputStateEvent, iv_socket_rpc_set_event_output_state_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcPauseEvent, iv_socket_rpc_pause_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcResumeEvent, iv_socket_rpc_resume_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcSaveProjectEvent, iv_socket_rpc_save_project_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcEnableProjectAutosaveEvent, iv_socket_rpc_enable_project_autosave_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcDisableProjectAutosaveEvent, iv_socket_rpc_disable_project_autosave_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcServerShutdownEvent, iv_socket_rpc_server_shutdown_event);

    class SocketRpcServer {
        std::filesystem::path workspace_root;
        int rpc_fd_value = -1;

        mutable std::mutex mutex;
        mutable std::mutex client_mutex;
        mutable std::mutex write_mutex;
        mutable std::mutex deferred_notification_mutex;
        mutable std::condition_variable ready_cv;
        mutable std::condition_variable stopped_cv;
        bool ready = false;
        bool stopped = false;
        int client_fd = -1;
        bool defer_current_request_notifications = false;
        std::thread::id deferred_notification_thread{};
        std::vector<std::string> deferred_notifications;
        std::optional<std::jthread> worker_thread;

        bool send_message(int fd, std::string const &message);
        void begin_deferring_current_request_notifications();
        std::vector<std::string> finish_deferring_current_request_notifications();
        void handle_client(int fd);

    public:
        explicit SocketRpcServer(
            std::filesystem::path workspace_root,
            int rpc_fd);
        ~SocketRpcServer();
        SocketRpcServer(SocketRpcServer &&) = delete;
        SocketRpcServer &operator=(SocketRpcServer &&) = delete;

        SocketRpcServer(SocketRpcServer const &) = delete;
        SocketRpcServer &operator=(SocketRpcServer const &) = delete;

        void start();
        bool wait_until_ready(std::chrono::milliseconds timeout) const;
        void wait();
        void request_shutdown();

        void send_server_message(SocketRpcServerMessage const &notification);
        void send_server_status(SocketRpcServerStatus const &notification);
        void send_lane_view_updated(LaneViewResult const &notification);
        void send_lane_view_content_updated(LaneViewContentUpdate const &notification);
        void send_iv_module_instances_updated(
            std::vector<IvModuleInstanceInfo> const &instances);
        void send_logical_nodes_updated(
            ProjectLogicalNodesNotification const &notification);
    };
} // namespace iv
