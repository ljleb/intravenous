#include "runtime/socket_rpc_server.h"

#include "filesystem_paths.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <type_traits>
#include <unistd.h>

namespace iv {
namespace {
using Json = nlohmann::ordered_json;

Json parse_request_json(std::string const &line) {
    try {
        return Json::parse(line);
    } catch (Json::parse_error const &e) {
        throw std::runtime_error(std::string("invalid JSON-RPC request: ") + e.what());
    }
}

std::string serialize_json_line(Json const &value) {
    return value.dump() + "\n";
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

Json const &parse_request_params(Json const &request) {
    auto const params_it = request.find("params");
    if (params_it == request.end() || !params_it->is_object()) {
        throw std::runtime_error("JSON-RPC request is missing params object");
    }
    return *params_it;
}

int parse_request_id(Json const &request) {
    auto const id_it = request.find("id");
    if (id_it == request.end() || !id_it->is_number_integer()) {
        throw std::runtime_error("JSON-RPC request is missing numeric id");
    }
    return id_it->get<int>();
}

std::string parse_request_method(Json const &request) {
    auto const method_it = request.find("method");
    if (method_it == request.end() || !method_it->is_string()) {
        throw std::runtime_error("JSON-RPC request is missing method");
    }
    return method_it->get<std::string>();
}

std::string parse_string_param(Json const &params, std::string const &key) {
    auto const value_it = params.find(key);
    if (value_it == params.end() || !value_it->is_string()) {
        throw std::runtime_error("JSON-RPC request is missing string param '" + key + "'");
    }
    return value_it->get<std::string>();
}

std::vector<std::string> parse_string_array_param(Json const &params, std::string const &key) {
    auto const value_it = params.find(key);
    if (value_it == params.end() || !value_it->is_array()) {
        throw std::runtime_error("JSON-RPC request is missing string array param '" + key + "'");
    }

    std::vector<std::string> values;
    values.reserve(value_it->size());
    for (auto const &item : *value_it) {
        if (!item.is_string()) {
            throw std::runtime_error("JSON-RPC request param '" + key + "' must be an array of strings");
        }
        values.push_back(item.get<std::string>());
    }
    return values;
}

uint64_t parse_uint64_param(Json const &params, std::string const &key) {
    auto const value_it = params.find(key);
    if (value_it == params.end() || !value_it->is_number_integer()) {
        throw std::runtime_error("JSON-RPC request is missing integer param '" + key + "'");
    }
    auto const value = value_it->get<int64_t>();
    if (value < 0) {
        throw std::runtime_error("JSON-RPC request param '" + key + "' must be non-negative");
    }
    return static_cast<uint64_t>(value);
}

std::optional<uint64_t> parse_optional_uint64_param(Json const &params, std::string const &key) {
    auto const value_it = params.find(key);
    if (value_it == params.end() || value_it->is_null()) {
        return std::nullopt;
    }
    if (!value_it->is_number_integer()) {
        throw std::runtime_error("JSON-RPC request param '" + key + "' must be a non-negative integer");
    }
    auto const value = value_it->get<int64_t>();
    if (value < 0) {
        throw std::runtime_error("JSON-RPC request param '" + key + "' must be non-negative");
    }
    return static_cast<uint64_t>(value);
}

size_t parse_optional_size_param(Json const &params, std::string const &key, size_t fallback) {
    auto const value_it = params.find(key);
    if (value_it == params.end() || value_it->is_null()) {
        return fallback;
    }
    if (!value_it->is_number_integer()) {
        throw std::runtime_error("JSON-RPC request param '" + key + "' must be a non-negative integer");
    }
    auto const value = value_it->get<int64_t>();
    if (value < 0) {
        throw std::runtime_error("JSON-RPC request param '" + key + "' must be non-negative");
    }
    return static_cast<size_t>(value);
}

double parse_number_param(Json const &params, std::string const &key) {
    auto const value_it = params.find(key);
    if (value_it == params.end() || !value_it->is_number()) {
        throw std::runtime_error("JSON-RPC request is missing numeric param '" + key + "'");
    }
    return value_it->get<double>();
}

uint32_t parse_uint32_value(Json const &value, std::string const &context) {
    if (!value.is_number_integer()) {
        throw std::runtime_error(context);
    }
    auto const parsed = value.get<int64_t>();
    if (parsed < 0) {
        throw std::runtime_error(context);
    }
    return static_cast<uint32_t>(parsed);
}

std::vector<SourceRange> parse_ranges(Json const &params, bool require_non_empty = true) {
    std::vector<SourceRange> ranges;
    auto const ranges_it = params.find("ranges");
    if (ranges_it != params.end()) {
        if (!ranges_it->is_array()) {
            throw std::runtime_error("graph.queryBySpans ranges must be an array");
        }
        ranges.reserve(ranges_it->size());
        for (auto const &range_json : *ranges_it) {
            if (!range_json.is_object()) {
                throw std::runtime_error("graph.queryBySpans range entries must be objects");
            }
            auto const start_it = range_json.find("start");
            auto const end_it = range_json.find("end");
            if (start_it == range_json.end() || end_it == range_json.end() ||
                !start_it->is_object() || !end_it->is_object()) {
                throw std::runtime_error("graph.queryBySpans ranges must include start and end positions");
            }

            auto const start_line_it = start_it->find("line");
            auto const start_column_it = start_it->find("column");
            auto const end_line_it = end_it->find("line");
            auto const end_column_it = end_it->find("column");
            if (start_line_it == start_it->end() ||
                start_column_it == start_it->end() ||
                end_line_it == end_it->end() ||
                end_column_it == end_it->end()) {
                throw std::runtime_error("graph.queryBySpans positions must use unsigned line/column values");
            }

            ranges.push_back(SourceRange{
                .start = {
                    .line = parse_uint32_value(*start_line_it, "graph.queryBySpans positions must use unsigned line/column values"),
                    .column = parse_uint32_value(*start_column_it, "graph.queryBySpans positions must use unsigned line/column values"),
                },
                .end = {
                    .line = parse_uint32_value(*end_line_it, "graph.queryBySpans positions must use unsigned line/column values"),
                    .column = parse_uint32_value(*end_column_it, "graph.queryBySpans positions must use unsigned line/column values"),
                },
            });
        }
    }
    if (require_non_empty && ranges.empty()) {
        throw std::runtime_error("graph.queryBySpans requires at least one range");
    }
    return ranges;
}

SourceRangeMatchMode parse_match_mode(Json const &params) {
    auto const mode_it = params.find("match");
    if (mode_it == params.end()) {
        return SourceRangeMatchMode::intersection;
    }
    if (!mode_it->is_string()) {
        throw std::runtime_error("graph.queryBySpans match must be 'union' or 'intersection'");
    }
    auto const mode = mode_it->get<std::string>();
    if (mode == "union") {
        return SourceRangeMatchMode::union_;
    }
    if (mode == "intersection") {
        return SourceRangeMatchMode::intersection;
    }
    throw std::runtime_error("graph.queryBySpans match must be 'union' or 'intersection'");
}

LaneQueryFilter parse_lane_query_filter(Json const &params) {
    auto const filter_it = params.find("filter");
    if (filter_it == params.end() || !filter_it->is_object()) {
        throw std::runtime_error("lane requests require a filter object");
    }
    auto const kind_it = filter_it->find("kind");
    if (kind_it == filter_it->end() || !kind_it->is_string()) {
        throw std::runtime_error("lane filter.kind must be a string");
    }
    return LaneQueryFilter{.kind = kind_it->get<std::string>()};
}

LaneQuery parse_lane_query(Json const &params) {
    return LaneQuery{.filter = parse_lane_query_filter(params)};
}

LaneViewRequest parse_lane_view_request(Json const &params) {
    return LaneViewRequest{
        .view_id = parse_string_param(params, "viewId"),
        .query = parse_lane_query(params),
        .start_index = parse_optional_size_param(params, "startIndex", 0),
        .visible_lane_count = parse_optional_size_param(params, "visibleLaneCount", 0),
    };
}

Json string_array_json(std::vector<std::string> const &values) {
    Json json = Json::array();
    for (auto const &value : values) {
        json.push_back(value);
    }
    return json;
}

std::string_view port_kind_json(PortKind kind) {
    switch (kind) {
    case PortKind::sample:
        return "sample";
    case PortKind::event:
        return "event";
    }
    return "sample";
}

std::string_view lane_domain_json(LaneDomain domain) {
    switch (domain) {
    case LaneDomain::compiled:
        return "compiled";
    case LaneDomain::realtime:
        return "realtime";
    }
    return "realtime";
}

Json lane_query_result_json(LaneQueryResult const &result) {
    Json json_lanes = Json::array();
    for (auto const &lane : result.lanes) {
        auto const &graph_input_port = lane.graph_input_port;
        Json port{
            {"logicalNodeId", graph_input_port.logical_node_id},
            {"portKind", std::string(port_kind_json(graph_input_port.port_kind))},
            {"portOrdinal", graph_input_port.port_ordinal},
            {"portName", graph_input_port.port_name},
            {"portType", graph_input_port.port_type},
        };
        if (graph_input_port.concrete_member_ordinal.has_value()) {
            port["memberOrdinal"] = *graph_input_port.concrete_member_ordinal;
        }
        json_lanes.push_back({
            {"laneId", lane.lane_id},
            {"domain", std::string(lane_domain_json(lane.domain))},
            {"graphInputPort", std::move(port)},
        });
    }

    Json json_connections = Json::array();
    for (auto const &connection : result.connections) {
        json_connections.push_back({
            {"sourceLaneId", connection.source_lane_id},
            {"targetLaneId", connection.target_lane_id},
            {"portKind", std::string(port_kind_json(connection.port_kind))},
            {"portOrdinal", connection.port_ordinal},
        });
    }

    return Json{
        {"startIndex", result.start_index},
        {"visibleLaneCount", result.visible_lane_count},
        {"totalLaneCount", result.total_lane_count},
        {"lanes", std::move(json_lanes)},
        {"connections", std::move(json_connections)},
    };
}

Json lane_view_result_json(LaneViewResult const &result) {
    Json json = lane_query_result_json(result.lanes);
    json["viewId"] = result.view_id;
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
} // namespace

IV_DEFINE_LINKER_EVENT(SocketRpcInitializeEvent, iv_socket_rpc_server_initialize_event)
IV_DEFINE_LINKER_EVENT(SocketRpcGraphQueryBySpansEvent, iv_socket_rpc_graph_query_by_spans_event)
IV_DEFINE_LINKER_EVENT(SocketRpcGraphQueryActiveRegionsEvent, iv_socket_rpc_graph_query_active_regions_event)
IV_DEFINE_LINKER_EVENT(SocketRpcGetLogicalNodeEvent, iv_socket_rpc_get_logical_node_event)
IV_DEFINE_LINKER_EVENT(SocketRpcGetLogicalNodesEvent, iv_socket_rpc_get_logical_nodes_event)
IV_DEFINE_LINKER_EVENT(SocketRpcOpenLaneViewEvent, iv_socket_rpc_open_lane_view_event)
IV_DEFINE_LINKER_EVENT(SocketRpcUpdateLaneViewEvent, iv_socket_rpc_update_lane_view_event)
IV_DEFINE_LINKER_EVENT(SocketRpcCloseLaneViewEvent, iv_socket_rpc_close_lane_view_event)
IV_DEFINE_LINKER_EVENT(SocketRpcSetSampleInputValueEvent, iv_socket_rpc_set_sample_input_value_event)
IV_DEFINE_LINKER_EVENT(SocketRpcClearSampleInputValueOverrideEvent, iv_socket_rpc_clear_sample_input_value_override_event)
IV_DEFINE_LINKER_EVENT(SocketRpcServerShutdownEvent, iv_socket_rpc_server_shutdown_event)

SocketRpcServer::SocketRpcServer(std::filesystem::path workspace_root_, std::filesystem::path socket_path_)
    : workspace_root(normalize_path(workspace_root_)),
      socket_path_value(std::move(socket_path_)),
      lock_path_value(socket_path_value.string() + ".lock") {}

SocketRpcServer::~SocketRpcServer() { request_shutdown(); }

void SocketRpcServer::acquire_workspace_lock() {
    std::filesystem::create_directories(lock_path_value.parent_path());
    lock_fd = ::open(lock_path_value.c_str(), O_CREAT | O_RDWR, 0600);
    if (lock_fd < 0) {
        throw std::runtime_error(std::string("failed to open server lock file: ") + std::strerror(errno));
    }
    if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(lock_fd);
        lock_fd = -1;
        throw std::runtime_error("another Intravenous server is already running for this workspace");
    }
}

void SocketRpcServer::release_workspace_lock() {
    if (lock_fd >= 0) {
        ::flock(lock_fd, LOCK_UN);
        ::close(lock_fd);
        lock_fd = -1;
    }
    std::error_code ec;
    std::filesystem::remove(lock_path_value, ec);
}

bool SocketRpcServer::send_message(int fd, std::string const &message) {
    std::scoped_lock write_lock(write_mutex);
    size_t written = 0;
    while (written < message.size()) {
        ssize_t count = ::send(fd, message.data() + written, message.size() - written, MSG_NOSIGNAL);
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
                auto const request = parse_request_json(line);
                request_id = parse_request_id(request);
                auto const method = parse_request_method(request);
                auto const &params = parse_request_params(request);

                if (method == "server.initialize") {
                    auto const requested_workspace = normalize_path(parse_string_param(params, "workspaceRoot"));
                    ServerInitializeRequest event_request{.workspace_root = requested_workspace};
                    SocketRpcInitializeResultBuilder builder;
                    IV_INVOKE_LINKER_EVENT(iv_socket_rpc_server_initialize_event, event_request, builder);
                    response = builder.build(request_id);
                } else if (method == "graph.queryBySpans") {
                    GraphQueryBySpansRequest event_request{
                        .file_path = parse_string_param(params, "filePath"),
                        .ranges = parse_ranges(params),
                        .match_mode = parse_match_mode(params),
                    };
                    SocketRpcGraphQueryResultBuilder builder;
                    IV_INVOKE_LINKER_EVENT(iv_socket_rpc_graph_query_by_spans_event, event_request, builder);
                    response = builder.build(request_id);
                } else if (method == "graph.queryActiveRegions") {
                    GraphQueryActiveRegionsRequest event_request{
                        .file_path = parse_string_param(params, "filePath"),
                    };
                    SocketRpcRegionQueryResultBuilder builder;
                    IV_INVOKE_LINKER_EVENT(iv_socket_rpc_graph_query_active_regions_event, event_request, builder);
                    response = builder.build(request_id);
                } else if (method == "graph.getLogicalNode") {
                    GetLogicalNodeRequest event_request{.node_id = parse_string_param(params, "nodeId")};
                    SocketRpcLogicalNodeResultBuilder builder;
                    IV_INVOKE_LINKER_EVENT(iv_socket_rpc_get_logical_node_event, event_request, builder);
                    response = builder.build(request_id);
                } else if (method == "graph.getLogicalNodes") {
                    GetLogicalNodesRequest event_request{.node_ids = parse_string_array_param(params, "nodeIds")};
                    SocketRpcLogicalNodesResultBuilder builder;
                    IV_INVOKE_LINKER_EVENT(iv_socket_rpc_get_logical_nodes_event, event_request, builder);
                    response = builder.build(request_id);
                } else if (method == "timeline.openLaneView") {
                    auto const event_request = parse_lane_view_request(params);
                    SocketRpcLaneViewResultBuilder builder;
                    IV_INVOKE_LINKER_EVENT(iv_socket_rpc_open_lane_view_event, event_request, builder);
                    response = builder.build(request_id);
                } else if (method == "timeline.updateLaneView") {
                    auto const event_request = parse_lane_view_request(params);
                    SocketRpcLaneViewResultBuilder builder;
                    IV_INVOKE_LINKER_EVENT(iv_socket_rpc_update_lane_view_event, event_request, builder);
                    response = builder.build(request_id);
                } else if (method == "timeline.closeLaneView") {
                    auto const view_id = parse_string_param(params, "viewId");
                    SocketRpcAckResponseBuilder builder;
                    IV_INVOKE_LINKER_EVENT(iv_socket_rpc_close_lane_view_event, view_id, builder);
                    response = builder.build(request_id);
                } else if (method == "graph.setSampleInputValue") {
                    auto const member_ordinal = parse_optional_uint64_param(params, "memberOrdinal");
                    SetSampleInputValueRequest event_request{
                        .node_id = parse_string_param(params, "nodeId"),
                        .input_ordinal = static_cast<size_t>(parse_uint64_param(params, "inputOrdinal")),
                        .value = static_cast<Sample>(parse_number_param(params, "value")),
                        .member_ordinal = member_ordinal.has_value()
                            ? std::optional<size_t>(static_cast<size_t>(*member_ordinal))
                            : std::nullopt,
                    };
                    SocketRpcAckResponseBuilder builder;
                    IV_INVOKE_LINKER_EVENT(iv_socket_rpc_set_sample_input_value_event, event_request, builder);
                    response = builder.build(request_id);
                } else if (method == "graph.clearSampleInputValueOverride") {
                    ClearSampleInputValueOverrideRequest event_request{
                        .node_id = parse_string_param(params, "nodeId"),
                        .member_ordinal = static_cast<size_t>(parse_uint64_param(params, "memberOrdinal")),
                        .input_ordinal = static_cast<size_t>(parse_uint64_param(params, "inputOrdinal")),
                    };
                    SocketRpcAckResponseBuilder builder;
                    IV_INVOKE_LINKER_EVENT(iv_socket_rpc_clear_sample_input_value_override_event, event_request, builder);
                    response = builder.build(request_id);
                } else if (method == "server.shutdown") {
                    ServerShutdownRequest event_request{};
                    SocketRpcAckResponseBuilder builder;
                    IV_INVOKE_LINKER_EVENT(iv_socket_rpc_server_shutdown_event, event_request, builder);
                    response = builder.build(request_id);
                    shutdown_after_response = true;
                } else {
                    throw std::runtime_error("unsupported JSON-RPC method: " + method);
                }
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

void SocketRpcServer::accept_loop(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        int accepted_fd = ::accept(listen_fd, nullptr, nullptr);
        if (accepted_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        {
            std::scoped_lock client_lock(client_mutex);
            client_fd = accepted_fd;
        }
        handle_client(accepted_fd);
        {
            std::scoped_lock client_lock(client_mutex);
            if (client_fd == accepted_fd) {
                client_fd = -1;
            }
        }
        ::close(accepted_fd);
        request_shutdown();
        break;
    }

    {
        std::scoped_lock lock(mutex);
        stopped = true;
    }
    stopped_cv.notify_all();
}

void SocketRpcServer::start() {
    acquire_workspace_lock();
    std::filesystem::create_directories(socket_path_value.parent_path());
    std::error_code ec;
    std::filesystem::remove(socket_path_value, ec);

    listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        throw std::runtime_error(std::string("failed to create socket: ") + std::strerror(errno));
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    auto const socket_string = socket_path_value.string();
    if (socket_string.size() >= sizeof(address.sun_path)) {
        throw std::runtime_error("socket path is too long: " + socket_string);
    }
    std::memcpy(address.sun_path, socket_string.c_str(), socket_string.size() + 1);

    if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        throw std::runtime_error(std::string("failed to bind socket: ") + std::strerror(errno));
    }
    if (::listen(listen_fd, 8) != 0) {
        throw std::runtime_error(std::string("failed to listen on socket: ") + std::strerror(errno));
    }

    {
        std::scoped_lock lock(mutex);
        ready = true;
    }
    ready_cv.notify_all();

    accept_thread.emplace([this](std::stop_token stop_token) { accept_loop(stop_token); });
}

void SocketRpcServer::request_shutdown() {
    {
        std::scoped_lock client_lock(client_mutex);
        if (client_fd >= 0) {
            ::shutdown(client_fd, SHUT_RDWR);
        }
    }
    if (listen_fd >= 0) {
        ::shutdown(listen_fd, SHUT_RDWR);
        ::close(listen_fd);
        listen_fd = -1;
    }
    std::error_code ec;
    std::filesystem::remove(socket_path_value, ec);
    if (accept_thread.has_value()) {
        accept_thread->request_stop();
    }
    release_workspace_lock();
}

bool SocketRpcServer::wait_until_ready(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex);
    return ready_cv.wait_for(lock, timeout, [&] { return ready; });
}

void SocketRpcServer::wait() {
    std::unique_lock lock(mutex);
    stopped_cv.wait(lock, [&] { return stopped; });
}

std::filesystem::path SocketRpcServer::socket_path() const {
    return socket_path_value;
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
} // namespace iv
