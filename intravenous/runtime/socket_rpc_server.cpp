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

        uint64_t parse_uint64_param(std::string const& line, std::string const& key)
        {
            std::regex const pattern("\"" + key + R"("\s*:\s*([0-9]+))");
            std::smatch match;
            if (!std::regex_search(line, match, pattern)) {
                throw std::runtime_error("JSON-RPC request is missing integer param '" + key + "'");
            }
            return static_cast<uint64_t>(std::stoull(match[1].str()));
        }

        std::vector<SourceRange> parse_ranges(std::string const& line)
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
            if (ranges.empty()) {
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

        std::string live_port_json(std::vector<LivePortInfo> const& ports)
        {
            std::string json = "[";
            bool first = true;
            for (auto const& port : ports) {
                if (!first) {
                    json += ",";
                }
                first = false;
                json += "{\"name\":\"" + escape_json(port.name) + "\",\"type\":\"" + escape_json(port.type) +
                    "\",\"connected\":" + std::string(port.connected ? "true" : "false") + "}";
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

        std::string live_node_json(LiveNodeInfo const& node)
        {
            return "{\"id\":\"" + escape_json(node.id) +
                "\",\"kind\":\"" + escape_json(node.kind) +
                "\",\"sourceSpans\":" + source_spans_json(node.source_spans) +
                ",\"sampleInputs\":" + live_port_json(node.sample_inputs) +
                ",\"sampleOutputs\":" + live_port_json(node.sample_outputs) +
                ",\"eventInputs\":" + live_port_json(node.event_inputs) +
                ",\"eventOutputs\":" + live_port_json(node.event_outputs) + "}";
        }

        std::string initialize_result_json(RuntimeProjectInitializeResult const& result)
        {
            return "{\"moduleRoot\":\"" + escape_json(result.module_root.generic_string()) +
                "\",\"moduleId\":\"" + escape_json(result.module_id) +
                "\",\"executionEpoch\":" + std::to_string(result.execution_epoch) +
                ",\"capabilities\":{\"queryBySpans\":true,\"getNode\":true}}";
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
                json += live_node_json(node);
            }
            json += "]}";
            return json;
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
            json += "}";
            return json;
        }
    }

    class SocketRpcServer::Impl {
    public:
        std::filesystem::path workspace_root;
        std::filesystem::path discovery_start;
        std::filesystem::path socket_path_value;
        AudioDeviceFactory audio_device_factory;
        RuntimeProjectService service;

        mutable std::mutex mutex;
        mutable std::mutex client_mutex;
        mutable std::mutex write_mutex;
        mutable std::condition_variable ready_cv;
        mutable std::condition_variable stopped_cv;
        bool ready = false;
        bool stopped = false;
        int listen_fd = -1;
        int client_fd = -1;
        std::optional<std::jthread> accept_thread;
        RuntimeProjectInitializeResult initialized_result;

        explicit Impl(
            std::filesystem::path workspace_root_,
            std::filesystem::path discovery_start_,
            std::filesystem::path socket_path_,
            AudioDeviceFactory audio_device_factory_
        ) :
            workspace_root(normalize_path(workspace_root_)),
            discovery_start(std::move(discovery_start_)),
            socket_path_value(socket_path_.empty() ? default_socket_path(workspace_root) : std::move(socket_path_)),
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
                    try {
                        request_id = parse_request_id(line);
                        auto const method = parse_request_method(line);

                        if (method == "server.initialize") {
                            auto const requested_workspace = normalize_path(parse_string_param(line, "workspaceRoot"));
                            if (requested_workspace != workspace_root) {
                                throw std::runtime_error("workspaceRoot does not match server workspace");
                            }
                            response = jsonrpc_result(request_id, initialize_result_json(initialized_result));
                        } else if (method == "graph.queryBySpans") {
                            auto const file_path = parse_string_param(line, "filePath");
                            auto const ranges = parse_ranges(line);
                            auto const match_mode = parse_match_mode(line);
                            response = jsonrpc_result(
                                request_id,
                                query_result_json(service.query_by_spans(file_path, ranges, match_mode))
                            );
                        } else if (method == "graph.getNode") {
                            auto const execution_epoch = parse_uint64_param(line, "executionEpoch");
                            auto const node_id = parse_string_param(line, "nodeId");
                            response = jsonrpc_result(request_id, live_node_json(service.get_node(execution_epoch, node_id)));
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
                }
                handle_client(accepted_fd);
                {
                    std::scoped_lock client_lock(client_mutex);
                    if (client_fd == accepted_fd) {
                        client_fd = -1;
                    }
                }
                ::close(accepted_fd);
            }

            {
                std::scoped_lock lock(mutex);
                stopped = true;
            }
            stopped_cv.notify_all();
        }

        void start()
        {
            initialized_result = service.initialize();

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
        }

        void request_shutdown()
        {
            service.request_shutdown();
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
            if (accept_thread.has_value()) {
                accept_thread->request_stop();
            }
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
