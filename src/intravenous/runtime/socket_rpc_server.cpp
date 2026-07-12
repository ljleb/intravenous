#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/socket_rpc_json_serialization.h>

#include <intravenous/filesystem_paths.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <csignal>
#include <sys/socket.h>
#include <type_traits>
#include <unistd.h>

namespace iv {
namespace {
using Json = nlohmann::ordered_json;

std::string serialize_json_line(Json const &value) {
    return value.dump() + "\n";
}

std::string_view channel_type_json(ChannelTypeId type) {
    switch (type) {
    case ChannelTypeId::mono:
        return "mono";
    case ChannelTypeId::stereo:
        return "stereo";
    case ChannelTypeId::count:
        break;
    }
    return "mono";
}

std::string jsonrpc_error(int id, int code, std::string const &message) {
    return serialize_json_line(Json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {{"code", code}, {"message", message}}},
    });
}

std::string jsonrpc_notification(std::string const &method, Json params_json) {
    return serialize_json_line(Json{
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", std::move(params_json)},
    });
}


Json string_array_json(std::vector<std::string> const &values) {
    Json json = Json::array();
    for (auto const &value : values) {
        json.push_back(value);
    }
    return json;
}

Json lane_view_content_update_json(LaneViewContentUpdate const &update) {
    Json json_lanes = Json::array();
    for (auto const &lane : update.lanes) {
        Json json_events = Json::array();
        for (auto const &event : lane.events) {
            Json json_event{
                {"time", event.time},
            };
            std::visit(
                [&]<typename T>(T const &value) {
                    using Event = std::remove_cvref_t<T>;
                    if constexpr (std::same_as<Event, TriggerEvent>) {
                        json_event["value"] = Json{{"type", "trigger"}};
                    } else if constexpr (std::same_as<Event, MidiEvent>) {
                        Json bytes = Json::array();
                        for (size_t i = 0; i < value.size; ++i) {
                            bytes.push_back(value.bytes[i]);
                        }
                        json_event["value"] = Json{
                            {"type", "midi"},
                            {"bytes", std::move(bytes)},
                        };
                    } else if constexpr (std::same_as<Event, BoundaryEvent>) {
                        json_event["value"] = Json{
                            {"type", "boundary"},
                            {"isBegin", value.is_begin},
                        };
                    } else if constexpr (std::same_as<Event, EmptyEvent>) {
                        json_event["value"] = Json{{"type", "empty"}};
                    }
                },
                event.value);
            json_events.push_back(std::move(json_event));
        }
        Json json_lane{
            {"laneId", lane.lane_id.str()},
            {"adapterType", lane.adapter_type},
            {"sampleChannelType", lane.sample_channel_type.has_value()
                ? Json(channel_type_json(*lane.sample_channel_type))
                : Json(nullptr)},
            {"events", std::move(json_events)},
        };
        if (lane.peak_level.has_value()) {
            json_lane["peakLevel"] = *lane.peak_level;
        }
        if (lane.secondary_peak_level.has_value()) {
            json_lane["secondaryPeakLevel"] = *lane.secondary_peak_level;
        }
        if (lane.event_count.has_value()) {
            json_lane["eventCount"] = *lane.event_count;
        }
        json_lanes.push_back(std::move(json_lane));
    }

    Json json{
        {"viewId", update.view_id.str()},
        {"lanes", std::move(json_lanes)},
    };
    if (update.playback_sample_index.has_value()) {
        json["playbackSampleIndex"] = *update.playback_sample_index;
    }
    return json;
}

Json server_message_json(SocketRpcServerMessage const &notification) {
    Json json = {
        {"level", notification.level},
        {"message", notification.message},
    };
    if (!notification.module_root.empty()) {
        json["moduleRoot"] = notification.module_root.generic_string();
    }
    return json;
}

Json server_status_json(SocketRpcServerStatus const &notification) {
    Json json = {
        {"level", notification.level},
        {"message", notification.message},
    };
    if (!notification.code.empty()) {
        json["code"] = notification.code;
    }
    if (!notification.module_root.empty()) {
        json["moduleRoot"] = notification.module_root.generic_string();
    }
    if (!notification.created_node_ids.empty()) {
        json["createdNodeIds"] = string_array_json(notification.created_node_ids);
    }
    if (!notification.deleted_node_ids.empty()) {
        json["deletedNodeIds"] = string_array_json(notification.deleted_node_ids);
    }
    return json;
}

Json logical_nodes_updated_json(ProjectLogicalNodesNotification const &notification) {
    Json json{
        {"nodes", logical_nodes_json(notification.nodes)},
    };
    if (!notification.replace_instance_ids.empty()) {
        json["replaceInstanceIds"] = string_array_json(notification.replace_instance_ids);
    }
    return json;
}
} // namespace

SocketRpcServer::SocketRpcServer(std::filesystem::path workspace_root_, int rpc_fd)
    : workspace_root(normalize_path(workspace_root_)),
      rpc_fd_value(rpc_fd) {}

SocketRpcServer::~SocketRpcServer() { request_shutdown(); }

bool SocketRpcServer::send_message(int fd, std::string const &message) {
    std::scoped_lock write_lock(write_mutex);
    size_t written = 0;
    while (written < message.size()) {
        ssize_t count = ::write(fd, message.data() + written, message.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return false;
        }
        written += static_cast<size_t>(count);
    }
    return true;
}

void SocketRpcServer::begin_deferring_current_request_notifications() {
    std::scoped_lock lock(deferred_notification_mutex);
    defer_current_request_notifications = true;
    deferred_notification_thread = std::this_thread::get_id();
    deferred_notifications.clear();
}

std::vector<std::string> SocketRpcServer::finish_deferring_current_request_notifications() {
    std::scoped_lock lock(deferred_notification_mutex);
    defer_current_request_notifications = false;
    deferred_notification_thread = {};
    return std::move(deferred_notifications);
}

void SocketRpcServer::handle_client(int fd) {
    std::string buffer;
    std::array<char, 4096> read_buffer{};

    if (!send_message(fd, jsonrpc_notification("server.ready", Json::object()))) {
        return;
    }
    {
        std::scoped_lock lock(mutex);
        ready = true;
    }
    ready_cv.notify_all();

    for (;;) {
        ssize_t count = ::read(fd, read_buffer.data(), read_buffer.size());
        if (count <= 0) {
            return;
        }
        buffer.append(read_buffer.data(), static_cast<size_t>(count));

        for (;;) {
            auto const newline = buffer.find('\n');
            if (newline == std::string::npos) {
                break;
            }

            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (line.empty()) {
                continue;
            }

            int request_id = 0;
            std::string response;
            bool shutdown_after_response = false;
            begin_deferring_current_request_notifications();
            try {
                auto const request = parse_socket_rpc_request(line);
                request_id = request.request_id;

                std::visit([&](auto const &event_request) {
                    using Request = std::remove_cvref_t<decltype(event_request)>;
                    if constexpr (std::same_as<Request, GraphQueryBySpansRequest>) {
                        SocketRpcGraphQueryResultBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_graph_query_by_spans_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, GraphQueryActiveRegionsRequest>) {
                        SocketRpcRegionQueryResultBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_graph_query_active_regions_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, GetLogicalNodeRequest>) {
                        SocketRpcLogicalNodeResultBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_get_logical_node_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, GetLogicalNodesRequest>) {
                        SocketRpcLogicalNodesResultBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_get_logical_nodes_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, CreateIvModuleInstanceRequest>) {
                        SocketRpcCreateIvModuleInstanceResultBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_create_iv_module_instance_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, GetIvModuleSourcesRequest>) {
                        SocketRpcIvModuleSourcesResultBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_get_iv_module_sources_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, CreateIvModuleSourceRequest>) {
                        SocketRpcIvModuleSourceResultBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_create_iv_module_source_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, GetIvModuleInstancesRequest>) {
                        SocketRpcIvModuleInstancesResultBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_get_iv_module_instances_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, DeleteIvModuleInstanceRequest>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_delete_iv_module_instance_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, UpdateIvModuleInstancesRequest>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(
                            iv_socket_rpc_update_iv_module_instances_event,
                            event_request,
                            builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, SetTimelineCompiledSampleCacheChunkSizeMultiplierRequest>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(
                            iv_socket_rpc_set_timeline_compiled_sample_cache_chunk_size_multiplier_event,
                            event_request,
                            builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, SetTimelineLaneSampleChannelTypeRequest>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(
                            iv_socket_rpc_set_timeline_lane_sample_channel_type_event,
                            event_request,
                            builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, GetAudioDevicesRequest>) {
                        SocketRpcAudioDevicesResultBuilder builder;
                        IV_INVOKE_LINKER_EVENT(
                            iv_socket_rpc_get_audio_devices_event,
                            event_request,
                            builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, SetAudioDevicesRequest>) {
                        SocketRpcAudioDevicesResultBuilder builder;
                        IV_INVOKE_LINKER_EVENT(
                            iv_socket_rpc_set_audio_devices_event,
                            event_request,
                            builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, OpenLaneViewRpcRequest>) {
                        SocketRpcLaneViewResultBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_open_lane_view_event, event_request.request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, UpdateLaneViewRpcRequest>) {
                        SocketRpcLaneViewResultBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_update_lane_view_event, event_request.request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, std::string>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_close_lane_view_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, SetSampleInputValueRequest>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_set_sample_input_value_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, SetSampleInputStateRequest>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_set_sample_input_state_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, SetEventInputStateRequest>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_set_event_input_state_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, SetSampleOutputStateRequest>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_set_sample_output_state_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, SetEventOutputStateRequest>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_set_event_output_state_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, PauseRequest>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_pause_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, ResumeRequest>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_resume_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, SeekRequest>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_seek_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, SaveProjectRequest>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_save_project_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, EnableProjectAutosaveRequest>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_enable_project_autosave_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, DisableProjectAutosaveRequest>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_disable_project_autosave_event, event_request, builder);
                        response = builder.build(request_id);
                    } else if constexpr (std::same_as<Request, ServerShutdownRequest>) {
                        SocketRpcAckResponseBuilder builder;
                        IV_INVOKE_LINKER_EVENT(iv_socket_rpc_server_shutdown_event, event_request, builder);
                        response = builder.build(request_id);
                        shutdown_after_response = true;
                    }
                }, request.payload);
            } catch (std::exception const &e) {
                response = jsonrpc_error(request_id, -32000, e.what());
            }

            if (shutdown_after_response) {
                (void)send_message(fd, response);
                (void)finish_deferring_current_request_notifications();
                request_shutdown();
                return;
            }
            if (!send_message(fd, response)) {
                (void)finish_deferring_current_request_notifications();
                return;
            }
            for (auto const &notification : finish_deferring_current_request_notifications()) {
                if (!send_message(fd, notification)) {
                    return;
                }
            }
        }
    }
}

void SocketRpcServer::start() {
    if (rpc_fd_value < 0) {
        throw std::runtime_error("SocketRpcServer requires a valid connected rpc fd");
    }

    {
        std::scoped_lock client_lock(client_mutex);
        client_fd = rpc_fd_value;
    }

    worker_thread.emplace([this](std::stop_token) {
        sigset_t blocked_signals;
        sigemptyset(&blocked_signals);
        sigaddset(&blocked_signals, SIGPIPE);
        int const mask_result = ::pthread_sigmask(SIG_BLOCK, &blocked_signals, nullptr);
        if (mask_result != 0) {
            {
                std::scoped_lock lock(mutex);
                stopped = true;
            }
            ready_cv.notify_all();
            stopped_cv.notify_all();
            return;
        }

        int fd = -1;
        {
            std::scoped_lock client_lock(client_mutex);
            fd = client_fd;
        }
        if (fd >= 0) {
            handle_client(fd);
            ::close(fd);
        }
        {
            std::scoped_lock client_lock(client_mutex);
            if (client_fd == fd) {
                client_fd = -1;
            }
        }
        {
            std::scoped_lock lock(mutex);
            stopped = true;
        }
        stopped_cv.notify_all();
    });
}

void SocketRpcServer::request_shutdown() {
    {
        std::scoped_lock client_lock(client_mutex);
        if (client_fd >= 0) {
            ::shutdown(client_fd, SHUT_RDWR);
        }
    }
    if (worker_thread.has_value()) {
        worker_thread->request_stop();
    }
}

bool SocketRpcServer::wait_until_ready(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex);
    return ready_cv.wait_for(lock, timeout, [&] { return ready || stopped; }) && ready;
}

void SocketRpcServer::wait() {
    std::unique_lock lock(mutex);
    stopped_cv.wait(lock, [&] { return stopped; });
}

void SocketRpcServer::send_server_message(SocketRpcServerMessage const &notification) {
    int fd = -1;
    {
        std::scoped_lock client_lock(client_mutex);
        fd = client_fd;
    }
    if (fd < 0) {
        return;
    }
    auto const message = jsonrpc_notification("server.message", server_message_json(notification));
    if (!send_message(fd, message)) {
        std::scoped_lock client_lock(client_mutex);
        if (client_fd == fd) {
            client_fd = -1;
        }
    }
}

void SocketRpcServer::send_server_status(SocketRpcServerStatus const &notification) {
    int fd = -1;
    {
        std::scoped_lock client_lock(client_mutex);
        fd = client_fd;
    }
    if (fd < 0) {
        return;
    }
    auto const message = jsonrpc_notification("server.status", server_status_json(notification));
    if (!send_message(fd, message)) {
        std::scoped_lock client_lock(client_mutex);
        if (client_fd == fd) {
            client_fd = -1;
        }
    }
}

void SocketRpcServer::send_lane_view_updated(LaneViewResult const &notification) {
    int fd = -1;
    {
        std::scoped_lock client_lock(client_mutex);
        fd = client_fd;
    }
    if (fd < 0) {
        return;
    }

    auto const message =
        jsonrpc_notification("timeline.laneViewUpdated", lane_view_result_json(notification));
    {
        std::scoped_lock deferred_lock(deferred_notification_mutex);
        if (defer_current_request_notifications &&
            deferred_notification_thread == std::this_thread::get_id()) {
            deferred_notifications.push_back(message);
            return;
        }
    }

    if (!send_message(fd, message)) {
        std::scoped_lock client_lock(client_mutex);
        if (client_fd == fd) {
            client_fd = -1;
        }
    }
}

void SocketRpcServer::send_lane_view_content_updated(
    LaneViewContentUpdate const &notification) {
    int fd = -1;
    {
        std::scoped_lock client_lock(client_mutex);
        fd = client_fd;
    }
    if (fd < 0) {
        return;
    }

    auto const message =
        jsonrpc_notification(
            "timeline.laneViewContentUpdated",
            lane_view_content_update_json(notification));
    {
        std::scoped_lock deferred_lock(deferred_notification_mutex);
        if (defer_current_request_notifications &&
            deferred_notification_thread == std::this_thread::get_id()) {
            deferred_notifications.push_back(message);
            return;
        }
    }

    if (!send_message(fd, message)) {
        std::scoped_lock client_lock(client_mutex);
        if (client_fd == fd) {
            client_fd = -1;
        }
    }
}

void SocketRpcServer::send_iv_module_instances_updated(
    std::vector<IvModuleInstanceInfo> const &instances) {
    int fd = -1;
    {
        std::scoped_lock client_lock(client_mutex);
        fd = client_fd;
    }
    if (fd < 0) {
        return;
    }

    auto const message = jsonrpc_notification(
        "ivModuleInstances.updated",
        Json{{"instances", iv_module_instances_json(instances)}});

    {
        std::scoped_lock deferred_lock(deferred_notification_mutex);
        if (defer_current_request_notifications &&
            deferred_notification_thread == std::this_thread::get_id()) {
            deferred_notifications.push_back(message);
            return;
        }
    }

    if (!send_message(fd, message)) {
        std::scoped_lock client_lock(client_mutex);
        if (client_fd == fd) {
            client_fd = -1;
        }
    }
}

void SocketRpcServer::send_logical_nodes_updated(
    ProjectLogicalNodesNotification const &notification) {
    int fd = -1;
    {
        std::scoped_lock client_lock(client_mutex);
        fd = client_fd;
    }
    if (fd < 0) {
        return;
    }

    auto const message = jsonrpc_notification(
        "graph.nodesUpdated",
        logical_nodes_updated_json(notification));

    {
        std::scoped_lock deferred_lock(deferred_notification_mutex);
        if (defer_current_request_notifications &&
            deferred_notification_thread == std::this_thread::get_id()) {
            deferred_notifications.push_back(message);
            return;
        }
    }

    if (!send_message(fd, message)) {
        std::scoped_lock client_lock(client_mutex);
        if (client_fd == fd) {
            client_fd = -1;
        }
    }
}
} // namespace iv
