#include "runtime/socket_rpc_server.h"

#include "compat.h"

#include <atomic>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace iv {
    namespace {
        std::string escape_json(std::string_view value)
        {
            std::string escaped;
            escaped.reserve(value.size());
            for (char c : value) {
                switch (c) {
                case '\\':
                    escaped += "\\\\";
                    break;
                case '"':
                    escaped += "\\\"";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                default:
                    escaped.push_back(c);
                    break;
                }
            }
            return escaped;
        }

        std::string unescape_json(std::string_view value)
        {
            std::string unescaped;
            unescaped.reserve(value.size());
            bool escape = false;
            for (char c : value) {
                if (escape) {
                    switch (c) {
                    case 'n':
                        unescaped.push_back('\n');
                        break;
                    case '\\':
                    case '"':
                        unescaped.push_back(c);
                        break;
                    default:
                        unescaped.push_back(c);
                        break;
                    }
                    escape = false;
                    continue;
                }
                if (c == '\\') {
                    escape = true;
                    continue;
                }
                unescaped.push_back(c);
            }
            return unescaped;
        }

        std::filesystem::path normalize_path(std::filesystem::path const& path)
        {
            return std::filesystem::weakly_canonical(path).lexically_normal();
        }

        std::filesystem::path default_socket_path(std::filesystem::path const& workspace_root)
        {
            auto const normalized = normalize_path(workspace_root).generic_string();
            uint64_t hash = 1469598103934665603ull;
            for (unsigned char c : normalized) {
                hash ^= c;
                hash *= 1099511628211ull;
            }

            auto const dir = std::filesystem::temp_directory_path() / "intravenous";
            std::filesystem::create_directories(dir);
            std::ostringstream out;
            out << std::hex << hash;
            return dir / ("workspace-" + out.str() + ".sock");
        }

        std::string jsonrpc_result(int id, std::string result_json)
        {
            return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":" + std::move(result_json) + "}\n";
        }

        std::string jsonrpc_error(int id, int code, std::string const& message)
        {
            return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"error\":{\"code\":" + std::to_string(code) +
                ",\"message\":\"" + escape_json(message) + "\"}}\n";
        }

        std::string jsonrpc_notification(std::string const& method, std::string params_json)
        {
            return "{\"jsonrpc\":\"2.0\",\"method\":\"" + escape_json(method) + "\",\"params\":" + std::move(params_json) + "}\n";
        }

        int parse_request_id(std::string const& line)
        {
            static std::regex const pattern(R"("id"\s*:\s*([0-9]+))");
            std::smatch match;
            if (!std::regex_search(line, match, pattern)) {
                throw std::runtime_error("JSON-RPC request is missing numeric id");
            }
            return std::stoi(match[1].str());
        }

        std::string parse_request_method(std::string const& line)
        {
            static std::regex const pattern("\"method\"\\s*:\\s*\"([^\"]+)\"");
            std::smatch match;
            if (!std::regex_search(line, match, pattern)) {
                throw std::runtime_error("JSON-RPC request is missing method");
            }
            return match[1].str();
        }

        std::string parse_string_param(std::string const& line, std::string const& key)
        {
            std::regex const pattern("\"" + key + "\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"");
            std::smatch match;
            if (!std::regex_search(line, match, pattern)) {
                throw std::runtime_error("JSON-RPC request is missing string param '" + key + "'");
            }
            return unescape_json(match[1].str());
        }

        std::vector<std::string> parse_string_array_param(std::string const& line, std::string const& key)
        {
            std::regex const pattern("\"" + key + "\"\\s*:\\s*\\[(.*?)\\]");
            std::smatch match;
            if (!std::regex_search(line, match, pattern)) {
                throw std::runtime_error("JSON-RPC request is missing string array param '" + key + "'");
            }

            std::vector<std::string> values;
            std::regex const item_pattern("\"((?:\\\\.|[^\"])*)\"");
            for (std::sregex_iterator it(match[1].first, match[1].second, item_pattern), end; it != end; ++it) {
                values.push_back(unescape_json((*it)[1].str()));
            }
            return values;
        }

        uint64_t parse_uint64_param(std::string const& line, std::string const& key)
        {
            std::regex const pattern("\"" + key + R"("\s*:\s*([0-9]+))");
            std::smatch match;
            if (!std::regex_search(line, match, pattern)) {
                throw std::runtime_error("JSON-RPC request is missing integer param '" + key + "'");
            }
            return static_cast<uint64_t>(std::stoull(match[1].str()));
        }

        std::vector<SourceRange> parse_ranges(std::string const& line, bool require_non_empty = true)
        {
            static std::regex const range_pattern(
                R"(\{"start"\s*:\s*\{"line"\s*:\s*([0-9]+)\s*,\s*"column"\s*:\s*([0-9]+)\}\s*,\s*"end"\s*:\s*\{"line"\s*:\s*([0-9]+)\s*,\s*"column"\s*:\s*([0-9]+)\}\})"
            );

            std::vector<SourceRange> ranges;
            for (std::sregex_iterator it(line.begin(), line.end(), range_pattern), end; it != end; ++it) {
                ranges.push_back(SourceRange {
                    .start = {
                        .line = static_cast<uint32_t>(std::stoul((*it)[1].str())),
                        .column = static_cast<uint32_t>(std::stoul((*it)[2].str())),
                    },
                    .end = {
                        .line = static_cast<uint32_t>(std::stoul((*it)[3].str())),
                        .column = static_cast<uint32_t>(std::stoul((*it)[4].str())),
                    },
                });
            }
            if (require_non_empty && ranges.empty()) {
                throw std::runtime_error("graph.queryBySpans requires at least one range");
            }
            return ranges;
        }

        SourceRangeMatchMode parse_match_mode(std::string const& line)
        {
            std::regex const pattern("\"match\"\\s*:\\s*\"([^\"]+)\"");
            std::smatch match;
            if (!std::regex_search(line, match, pattern)) {
                return SourceRangeMatchMode::intersection;
            }

            auto const mode = match[1].str();
            if (mode == "union") {
                return SourceRangeMatchMode::union_;
            }
            if (mode == "intersection") {
                return SourceRangeMatchMode::intersection;
            }
            throw std::runtime_error("graph.queryBySpans match must be 'union' or 'intersection'");
        }

        std::string connectivity_json(LogicalPortConnectivity connectivity)
        {
            switch (connectivity) {
            case LogicalPortConnectivity::connected:
                return "connected";
            case LogicalPortConnectivity::mixed:
                return "mixed";
            case LogicalPortConnectivity::disconnected:
                return "disconnected";
            }
            return "disconnected";
        }

        std::string logical_port_json(std::vector<LogicalPortInfo> const& ports)
        {
            std::string json = "[";
            bool first = true;
            for (auto const& port : ports) {
                if (!first) {
                    json += ",";
                }
                first = false;
                json += "{\"name\":\"" + escape_json(port.name) + "\",\"type\":\"" + escape_json(port.type) +
                    "\",\"connectivity\":\"" + connectivity_json(port.connectivity) + "\"}";
            }
            json += "]";
            return json;
        }

        std::string source_spans_json(std::vector<LiveSourceSpan> const& spans)
        {
            std::string json = "[";
            bool first = true;
            for (auto const& span : spans) {
                if (!first) {
                    json += ",";
                }
                first = false;
                json += "{\"filePath\":\"" + escape_json(span.file_path) +
                    "\",\"start\":{\"line\":" + std::to_string(span.range.start.line) +
                    ",\"column\":" + std::to_string(span.range.start.column) +
                    "},\"end\":{\"line\":" + std::to_string(span.range.end.line) +
                    ",\"column\":" + std::to_string(span.range.end.column) + "}}";
            }
            json += "]";
            return json;
        }

        std::string member_nodes_json(std::vector<LogicalNodeMemberInfo> const& member_nodes)
        {
            std::string json = "[";
            bool first = true;
            for (auto const& member : member_nodes) {
                if (!first) {
                    json += ",";
                }
                first = false;
                json += "{\"id\":\"" + escape_json(member.id) +
                    "\",\"kind\":\"" + escape_json(member.kind) + "\"}";
            }
            json += "]";
            return json;
        }

        std::string string_array_json(std::vector<std::string> const& values)
        {
            std::string json = "[";
            bool first = true;
            for (auto const& value : values) {
                if (!first) {
                    json += ",";
                }
                first = false;
                json += "\"" + escape_json(value) + "\"";
            }
            json += "]";
            return json;
        }

        std::string logical_node_json(LogicalNodeInfo const& node)
        {
            return "{\"id\":\"" + escape_json(node.id) +
                "\",\"kind\":\"" + escape_json(node.kind) +
                "\",\"sourceSpans\":" + source_spans_json(node.source_spans) +
                ",\"sampleInputs\":" + logical_port_json(node.sample_inputs) +
                ",\"sampleOutputs\":" + logical_port_json(node.sample_outputs) +
                ",\"eventInputs\":" + logical_port_json(node.event_inputs) +
                ",\"eventOutputs\":" + logical_port_json(node.event_outputs) +
                ",\"memberNodes\":" + member_nodes_json(node.member_nodes) +
                ",\"memberCount\":" + std::to_string(node.member_count) + "}";
        }

        std::string logical_nodes_json(std::vector<LogicalNodeInfo> const& nodes)
        {
            std::string json = "[";
            bool first = true;
            for (auto const& node : nodes) {
                if (!first) {
                    json += ",";
                }
                first = false;
                json += logical_node_json(node);
            }
            json += "]";
            return json;
        }

        std::string initialize_result_json(RuntimeProjectInitializeResult const& result)
        {
            return "{\"moduleRoot\":\"" + escape_json(result.module_root.generic_string()) +
                "\",\"moduleId\":\"" + escape_json(result.module_id) +
                "\",\"executionEpoch\":" + std::to_string(result.execution_epoch) +
                ",\"capabilities\":{\"queryBySpans\":true,\"queryActiveRegions\":true,\"getLogicalNode\":true,\"getLogicalNodes\":true}}";
        }

        std::string query_result_json(RuntimeProjectQueryResult const& result)
        {
            std::string json = "{\"executionEpoch\":" + std::to_string(result.execution_epoch) + ",\"nodes\":[";
            bool first = true;
            for (auto const& node : result.nodes) {
                if (!first) {
                    json += ",";
                }
                first = false;
                json += logical_node_json(node);
            }
            json += "]}";
            return json;
        }

        std::string region_query_result_json(RuntimeProjectRegionQueryResult const& result)
        {
            return "{\"executionEpoch\":" + std::to_string(result.execution_epoch) +
                ",\"sourceSpans\":" + source_spans_json(result.source_spans) + "}";
        }

        std::string runtime_event_params_json(RuntimeProjectEvent const& event)
        {
            std::string json = "{\"level\":\"" + escape_json(event.level) +
                "\",\"message\":\"" + escape_json(event.message) + "\"";
            if (!event.module_root.empty()) {
                json += ",\"moduleRoot\":\"" + escape_json(event.module_root.generic_string()) + "\"";
            }
            if (event.execution_epoch != 0) {
                json += ",\"executionEpoch\":" + std::to_string(event.execution_epoch);
            }
            if (!event.created_node_ids.empty()) {
                json += ",\"createdNodeIds\":" + string_array_json(event.created_node_ids);
            }
            if (!event.deleted_node_ids.empty()) {
                json += ",\"deletedNodeIds\":" + string_array_json(event.deleted_node_ids);
            }
            json += "}";
            return json;
        }
    }

    class SocketRpcServer::Impl {
    public:
        std::filesystem::path workspace_root;
        std::filesystem::path discovery_start;
        std::filesystem::path socket_path_value;
        std::filesystem::path lock_path_value;
        AudioDeviceFactory audio_device_factory;
        RuntimeProjectService service;

        mutable std::mutex mutex;
        mutable std::mutex client_mutex;
        mutable std::mutex write_mutex;
        mutable std::condition_variable ready_cv;
        mutable std::condition_variable stopped_cv;
        mutable std::condition_variable initialized_cv;
        bool ready = false;
        bool stopped = false;
        bool initialized = false;
        int listen_fd = -1;
        int client_fd = -1;
        bool client_initialized = false;
        int lock_fd = -1;
        std::optional<std::jthread> accept_thread;
        std::optional<std::jthread> initialize_thread;
        RuntimeProjectInitializeResult initialized_result;
        std::exception_ptr initialization_exception;

        explicit Impl(
            std::filesystem::path workspace_root_,
            std::filesystem::path discovery_start_,
            std::filesystem::path socket_path_,
            AudioDeviceFactory audio_device_factory_
        ) :
            workspace_root(normalize_path(workspace_root_)),
            discovery_start(std::move(discovery_start_)),
            socket_path_value(socket_path_.empty() ? default_socket_path(workspace_root) : std::move(socket_path_)),
            lock_path_value(socket_path_value.string() + ".lock"),
            audio_device_factory(std::move(audio_device_factory_)),
            service(
                workspace_root,
                discovery_start,
                {},
                audio_device_factory,
                [this](RuntimeProjectEvent const& event) {
                    notify_event(event);
                }
            )
        {}

        ~Impl()
        {
            request_shutdown();
        }

        void acquire_workspace_lock()
        {
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

        void release_workspace_lock()
        {
            if (lock_fd >= 0) {
                ::flock(lock_fd, LOCK_UN);
                ::close(lock_fd);
                lock_fd = -1;
            }
            std::error_code ec;
            std::filesystem::remove(lock_path_value, ec);
        }

        bool send_message(int fd, std::string const& message)
        {
            std::scoped_lock write_lock(write_mutex);
            size_t written = 0;
            while (written < message.size()) {
                ssize_t count = ::write(fd, message.data() + written, message.size() - written);
                if (count < 0) {
                    return false;
                }
                written += static_cast<size_t>(count);
            }
            return true;
        }

        void notify_event(RuntimeProjectEvent const& event)
        {
            std::string method;
            switch (event.kind) {
            case RuntimeProjectEventKind::log:
                method = "server.log";
                break;
            case RuntimeProjectEventKind::build_started:
                method = "server.buildStarted";
                break;
            case RuntimeProjectEventKind::build_finished:
                method = "server.buildFinished";
                break;
            case RuntimeProjectEventKind::build_failed:
                method = "server.buildFailed";
                break;
            }

            int fd = -1;
            {
                std::scoped_lock client_lock(client_mutex);
                fd = client_fd;
                if (!client_initialized) {
                    return;
                }
            }
            if (fd < 0) {
                return;
            }

            auto const message = jsonrpc_notification(method, runtime_event_params_json(event));
            if (!send_message(fd, message)) {
                std::scoped_lock client_lock(client_mutex);
                if (client_fd == fd) {
                    client_fd = -1;
                }
            }
        }

        void handle_client(int fd)
        {
            std::string buffer;
            std::array<char, 4096> read_buffer {};

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
                    bool mark_client_initialized = false;
                    try {
                        request_id = parse_request_id(line);
                        auto const method = parse_request_method(line);

                        if (method == "server.initialize") {
                            wait_until_initialized();
                            auto const requested_workspace = normalize_path(parse_string_param(line, "workspaceRoot"));
                            if (requested_workspace != workspace_root) {
                                throw std::runtime_error("workspaceRoot does not match server workspace");
                            }
                            response = jsonrpc_result(request_id, initialize_result_json(initialized_result));
                            mark_client_initialized = true;
                        } else if (method == "graph.queryBySpans") {
                            wait_until_initialized();
                            auto const file_path = parse_string_param(line, "filePath");
                            auto const ranges = parse_ranges(line);
                            auto const match_mode = parse_match_mode(line);
                            response = jsonrpc_result(
                                request_id,
                                query_result_json(service.query_by_spans(file_path, ranges, match_mode))
                            );
                        } else if (method == "graph.queryActiveRegions") {
                            wait_until_initialized();
                            auto const file_path = parse_string_param(line, "filePath");
                            response = jsonrpc_result(
                                request_id,
                                region_query_result_json(service.query_active_regions(file_path))
                            );
                        } else if (method == "graph.getLogicalNode") {
                            wait_until_initialized();
                            auto const execution_epoch = parse_uint64_param(line, "executionEpoch");
                            auto const node_id = parse_string_param(line, "nodeId");
                            response = jsonrpc_result(request_id, logical_node_json(service.get_logical_node(execution_epoch, node_id)));
                        } else if (method == "graph.getLogicalNodes") {
                            wait_until_initialized();
                            auto const execution_epoch = parse_uint64_param(line, "executionEpoch");
                            auto const node_ids = parse_string_array_param(line, "nodeIds");
                            response = jsonrpc_result(request_id, logical_nodes_json(service.get_logical_nodes(execution_epoch, node_ids)));
                        } else if (method == "server.shutdown") {
                            response = jsonrpc_result(request_id, "{\"ok\":true}");
                            (void)send_message(fd, response);
                            request_shutdown();
                            return;
                        } else {
                            throw std::runtime_error("unsupported JSON-RPC method: " + method);
                        }
                    } catch (std::exception const& e) {
                        response = jsonrpc_error(request_id, -32000, e.what());
                    }

                    if (!send_message(fd, response)) {
                        return;
                    }
                    if (mark_client_initialized) {
                        std::scoped_lock client_lock(client_mutex);
                        if (client_fd == fd) {
                            client_initialized = true;
                        }
                    }
                }
            }
        }

        void accept_loop(std::stop_token stop_token)
        {
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
                    client_initialized = false;
                }
                handle_client(accepted_fd);
                {
                    std::scoped_lock client_lock(client_mutex);
                    if (client_fd == accepted_fd) {
                        client_fd = -1;
                        client_initialized = false;
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

        void wait_until_initialized()
        {
            std::unique_lock lock(mutex);
            initialized_cv.wait(lock, [&] {
                return initialized || initialization_exception != nullptr;
            });
            if (initialization_exception != nullptr) {
                std::rethrow_exception(initialization_exception);
            }
        }

        void start()
        {
            acquire_workspace_lock();
            std::filesystem::create_directories(socket_path_value.parent_path());
            std::error_code ec;
            std::filesystem::remove(socket_path_value, ec);

            listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (listen_fd < 0) {
                throw std::runtime_error(std::string("failed to create socket: ") + std::strerror(errno));
            }

            sockaddr_un address {};
            address.sun_family = AF_UNIX;
            auto const socket_string = socket_path_value.string();
            if (socket_string.size() >= sizeof(address.sun_path)) {
                throw std::runtime_error("socket path is too long: " + socket_string);
            }
            std::memcpy(address.sun_path, socket_string.c_str(), socket_string.size() + 1);

            if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
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

            accept_thread.emplace([this](std::stop_token stop_token) {
                accept_loop(stop_token);
            });

            initialize_thread.emplace([this](std::stop_token) {
                try {
                    auto result = service.initialize();
                    {
                        std::scoped_lock lock(mutex);
                        initialized_result = std::move(result);
                        initialized = true;
                    }
                } catch (...) {
                    {
                        std::scoped_lock lock(mutex);
                        initialization_exception = std::current_exception();
                    }
                    request_shutdown();
                }
                initialized_cv.notify_all();
            });
        }

        void request_shutdown()
        {
            service.request_shutdown();
            {
                std::scoped_lock client_lock(client_mutex);
                if (client_fd >= 0) {
                    ::shutdown(client_fd, SHUT_RDWR);
                }
                client_initialized = false;
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
            if (initialize_thread.has_value()) {
                initialize_thread->request_stop();
            }
            release_workspace_lock();
        }
    };

    SocketRpcServer::SocketRpcServer(
        std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::filesystem::path socket_path,
        AudioDeviceFactory audio_device_factory
    ) :
        _impl(std::make_unique<Impl>(
            std::move(workspace_root),
            std::move(discovery_start),
            std::move(socket_path),
            std::move(audio_device_factory)
        ))
    {}

    SocketRpcServer::~SocketRpcServer() = default;
    SocketRpcServer::SocketRpcServer(SocketRpcServer&&) noexcept = default;
    SocketRpcServer& SocketRpcServer::operator=(SocketRpcServer&&) noexcept = default;

    void SocketRpcServer::start()
    {
        _impl->start();
    }

    bool SocketRpcServer::wait_until_ready(std::chrono::milliseconds timeout) const
    {
        std::unique_lock lock(_impl->mutex);
        return _impl->ready_cv.wait_for(lock, timeout, [&] {
            return _impl->ready;
        });
    }

    void SocketRpcServer::wait()
    {
        std::unique_lock lock(_impl->mutex);
        _impl->stopped_cv.wait(lock, [&] {
            return _impl->stopped;
        });
    }

    std::filesystem::path SocketRpcServer::socket_path() const
    {
        return _impl->socket_path_value;
    }

    void SocketRpcServer::request_shutdown()
    {
        _impl->request_shutdown();
    }
}
