#pragma once

#include "graph/build_types.h"
#include "linker_event.h"
#include "runtime/lane_view_service.h"
#include "runtime/runtime_project_api_types.h"
#include "runtime/socket_rpc_response_builders.h"
#include "sample.h"

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
    struct ServerInitializeRequest {
        std::filesystem::path workspace_root{};
    };

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

    using SocketRpcInitializeResult = RuntimeProjectInitializeResult;
    using SocketRpcGraphQueryResult = RuntimeProjectQueryResult;
    using SocketRpcRegionQueryResult = RuntimeProjectRegionQueryResult;
    using SocketRpcServerMessage = RuntimeProjectMessageNotification;
    using SocketRpcServerStatus = RuntimeProjectStatusNotification;

    using SocketRpcInitializeEvent =
        void (*)(ServerInitializeRequest const &, SocketRpcInitializeResultBuilder &);
    using SocketRpcGraphQueryBySpansEvent =
        void (*)(GraphQueryBySpansRequest const &, SocketRpcGraphQueryResultBuilder &);
    using SocketRpcGraphQueryActiveRegionsEvent =
        void (*)(GraphQueryActiveRegionsRequest const &, SocketRpcRegionQueryResultBuilder &);
    using SocketRpcGetLogicalNodeEvent =
        void (*)(GetLogicalNodeRequest const &, SocketRpcLogicalNodeResultBuilder &);
    using SocketRpcGetLogicalNodesEvent =
        void (*)(GetLogicalNodesRequest const &, SocketRpcLogicalNodesResultBuilder &);
    using SocketRpcOpenLaneViewEvent =
        void (*)(LaneViewRequest const &, SocketRpcLaneViewResultBuilder &);
    using SocketRpcUpdateLaneViewEvent =
        void (*)(LaneViewRequest const &, SocketRpcLaneViewResultBuilder &);
    using SocketRpcCloseLaneViewEvent =
        void (*)(std::string const &, SocketRpcAckResponseBuilder &);
    using SocketRpcSetSampleInputValueEvent =
        void (*)(SetSampleInputValueRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcClearSampleInputValueOverrideEvent =
        void (*)(ClearSampleInputValueOverrideRequest const &, SocketRpcAckResponseBuilder &);
    using SocketRpcServerShutdownEvent =
        void (*)(ServerShutdownRequest const &, SocketRpcAckResponseBuilder &);

    IV_DECLARE_LINKER_EVENT(SocketRpcInitializeEvent, iv_socket_rpc_server_initialize_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGraphQueryBySpansEvent, iv_socket_rpc_graph_query_by_spans_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGraphQueryActiveRegionsEvent, iv_socket_rpc_graph_query_active_regions_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGetLogicalNodeEvent, iv_socket_rpc_get_logical_node_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcGetLogicalNodesEvent, iv_socket_rpc_get_logical_nodes_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcOpenLaneViewEvent, iv_socket_rpc_open_lane_view_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcUpdateLaneViewEvent, iv_socket_rpc_update_lane_view_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcCloseLaneViewEvent, iv_socket_rpc_close_lane_view_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcSetSampleInputValueEvent, iv_socket_rpc_set_sample_input_value_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcClearSampleInputValueOverrideEvent, iv_socket_rpc_clear_sample_input_value_override_event);
    IV_DECLARE_LINKER_EVENT(SocketRpcServerShutdownEvent, iv_socket_rpc_server_shutdown_event);

    class SocketRpcServer {
        std::filesystem::path workspace_root;
        std::filesystem::path socket_path_value;
        std::filesystem::path lock_path_value;

        mutable std::mutex mutex;
        mutable std::mutex client_mutex;
        mutable std::mutex write_mutex;
        mutable std::mutex deferred_notification_mutex;
        mutable std::condition_variable ready_cv;
        mutable std::condition_variable stopped_cv;
        bool ready = false;
        bool stopped = false;
        int listen_fd = -1;
        int client_fd = -1;
        bool defer_current_request_notifications = false;
        std::thread::id deferred_notification_thread{};
        std::vector<std::string> deferred_notifications;
        int lock_fd = -1;
        std::optional<std::jthread> accept_thread;

        void acquire_workspace_lock();
        void release_workspace_lock();
        bool send_message(int fd, std::string const &message);
        void begin_deferring_current_request_notifications();
        std::vector<std::string> finish_deferring_current_request_notifications();
        void handle_client(int fd);
        void accept_loop(std::stop_token stop_token);

    public:
        explicit SocketRpcServer(
            std::filesystem::path workspace_root,
            std::filesystem::path socket_path);
        ~SocketRpcServer();
        SocketRpcServer(SocketRpcServer &&) = delete;
        SocketRpcServer &operator=(SocketRpcServer &&) = delete;

        SocketRpcServer(SocketRpcServer const &) = delete;
        SocketRpcServer &operator=(SocketRpcServer const &) = delete;

        void start();
        bool wait_until_ready(std::chrono::milliseconds timeout) const;
        void wait();
        std::filesystem::path socket_path() const;
        void request_shutdown();

        void send_server_message(SocketRpcServerMessage const &notification);
        void send_server_status(SocketRpcServerStatus const &notification);
        void send_lane_view_updated(LaneViewResult const &notification);
    };
} // namespace iv
