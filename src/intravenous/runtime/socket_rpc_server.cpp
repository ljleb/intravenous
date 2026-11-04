#include <intravenous/runtime/socket_rpc_server.h>

#include <intravenous/filesystem_paths.h>
#include <intravenous/runtime/socket_rpc_json_serialization.h>
#include <intravenous/runtime/socket_rpc_routes.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace iv {
namespace {
using Json = nlohmann::ordered_json;

constexpr std::size_t max_client_outbound_bytes = 1024 * 1024;
constexpr std::size_t max_replay_entries = 2048;
// A complete replay is enqueued after its subscription response. Keep the
// replay budget below the per-client queue budget so that a valid replay does
// not look like backpressure from a slow client.
constexpr std::size_t max_replay_bytes = 512 * 1024;
constexpr std::uint32_t session_protocol_version = 1;

std::string project_path_hash(std::filesystem::path const& path)
{
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    auto hash = offset;
    for (auto const byte : path.generic_string()) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= prime;
    }
    std::ostringstream result;
    result << std::hex << std::setfill('0') << std::setw(16) << hash;
    return result.str();
}

std::filesystem::path session_runtime_root()
{
    if (auto const* value = std::getenv("XDG_RUNTIME_DIR"); value && *value) {
        return std::filesystem::path(value) / "intravenous";
    }
    return std::filesystem::path("/tmp")
        / ("intravenous-" + std::to_string(::getuid()));
}

std::string random_capability()
{
    std::random_device source;
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < 4; ++i) {
        result << std::setw(16)
               << (static_cast<std::uint64_t>(source()) << 32
                   | static_cast<std::uint64_t>(source()));
    }
    return result.str();
}

std::int64_t unix_time_milliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string json_line(Json const& value) { return value.dump() + "\n"; }

std::string jsonrpc_result(int id, Json result)
{
    return json_line(Json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", std::move(result)},
    });
}

std::string jsonrpc_error(int id, int code, std::string message)
{
    return json_line(Json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {{"code", code}, {"message", std::move(message)}}},
    });
}

std::string jsonrpc_notification(std::string const& method, Json params)
{
    return json_line(Json{
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", std::move(params)},
    });
}

Json string_array_json(std::vector<std::string> const& values)
{
    auto result = Json::array();
    for (auto const& value : values) result.push_back(value);
    return result;
}

Json server_message_json(SocketRpcServerMessage const& notification)
{
    Json result{
        {"level", notification.level},
        {"message", notification.message},
    };
    if (!notification.package_root.empty()) {
        result["packageRoot"] = notification.package_root.generic_string();
    }
    return result;
}

Json server_status_json(SocketRpcServerStatus const& notification)
{
    Json result{
        {"level", notification.level},
        {"message", notification.message},
    };
    if (!notification.code.empty()) result["code"] = notification.code;
    if (!notification.package_root.empty()) {
        result["packageRoot"] = notification.package_root.generic_string();
    }
    if (!notification.created_node_ids.empty()) {
        result["createdNodeIds"] = string_array_json(notification.created_node_ids);
    }
    if (!notification.deleted_node_ids.empty()) {
        result["deletedNodeIds"] = string_array_json(notification.deleted_node_ids);
    }
    return result;
}

Json virtual_nodes_updated_json(ProjectVirtualNodesNotification const& notification)
{
    Json result{{"nodes", virtual_nodes_json(notification.nodes)}};
    if (!notification.replace_instance_ids.empty()) {
        result["replaceInstanceIds"] =
            string_array_json(notification.replace_instance_ids);
    }
    return result;
}

bool write_all(int fd, std::string const& message)
{
    std::size_t written = 0;
    while (written < message.size()) {
        auto const count = ::send(
            fd,
            message.data() + written,
            message.size() - written,
            MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) return false;
        written += static_cast<std::size_t>(count);
    }
    return true;
}

bool response_succeeded(std::string const& response)
{
    return !Json::parse(response).contains("error");
}

std::string stamp_response(std::string const& response, std::uint64_t revision)
{
    auto value = Json::parse(response);
    if (auto result = value.find("result"); result != value.end()) {
        if (!result->is_object()) *result = Json{{"value", *result}};
        (*result)["sessionRevision"] = revision;
    } else if (auto error = value.find("error"); error != value.end()) {
        if (!error->is_object()) *error = Json::object();
        (*error)["data"]["currentRevision"] = revision;
    }
    return json_line(value);
}
} // namespace

struct SocketRpcServer::Impl {
    struct OutboundMessage {
        std::string value{};
        bool shutdown_after_write = false;
    };

    struct Client : std::enable_shared_from_this<Client> {
        std::uint64_t key = 0;
        int fd = -1;
        bool authenticated = false;
        bool subscribed = false;
        std::string identity{};
        std::unordered_set<std::string> capabilities{};
        std::unordered_set<std::string> topics{};
        mutable std::mutex mutex{};
        std::condition_variable queue_ready{};
        std::deque<OutboundMessage> outbound{};
        std::size_t outbound_bytes = 0;
        bool closing = false;
        std::optional<std::jthread> reader{};
        std::optional<std::jthread> writer{};

        ~Client()
        {
            {
                std::scoped_lock lock(mutex);
                closing = true;
            }
            queue_ready.notify_all();
            if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
            if (reader) reader->request_stop();
            if (writer) writer->request_stop();
            reader.reset();
            writer.reset();
            if (fd >= 0) ::close(fd);
        }
    };

    struct Command {
        std::shared_ptr<Client> client{};
        std::string line{};
        std::string notification_method{};
        Json notification_params{};
        std::uint64_t disconnected_client_key = 0;

        [[nodiscard]] bool is_notification() const noexcept
        {
            return !notification_method.empty();
        }

        [[nodiscard]] bool is_client_cleanup() const noexcept
        {
            return disconnected_client_key != 0;
        }
    };

    struct PendingNotification {
        std::string method{};
        Json params{};
    };

    struct ReplayEntry {
        std::uint64_t revision = 0;
        std::string method{};
        std::string message{};
    };

    std::filesystem::path workspace_root{};
    int initial_rpc_fd = -1;
    SocketRpcRouteRegistry routes{};
    int listen_fd = -1;
    int lock_fd = -1;
    bool owns_session_lock = false;
    std::filesystem::path session_directory{};
    std::filesystem::path session_endpoint{};
    std::filesystem::path session_descriptor{};
    std::string local_capability{};
    std::optional<std::jthread> accept_thread{};

    mutable std::mutex lifecycle_mutex{};
    std::condition_variable ready_cv{};
    std::condition_variable stopped_cv{};
    bool started = false;
    bool ready = false;
    bool stopped = false;
    std::atomic<bool> shutting_down = false;

    mutable std::mutex clients_mutex{};
    std::unordered_map<std::uint64_t, std::shared_ptr<Client>> clients{};
    std::uint64_t next_client_key = 1;

    std::mutex commands_mutex{};
    std::condition_variable commands_ready{};
    std::deque<Command> commands{};
    std::optional<std::jthread> command_thread{};

    std::mutex deferred_mutex{};
    bool deferring_notifications = false;
    std::thread::id deferring_thread{};
    std::vector<PendingNotification> deferred_notifications{};

    mutable std::mutex session_mutex{};
    std::uint64_t revision = 0;
    std::uint64_t replay_floor = 0;
    std::deque<ReplayEntry> replay{};
    std::size_t replay_bytes = 0;

    Impl(std::filesystem::path root, int fd)
        : workspace_root(normalize_path(root)), initial_rpc_fd(fd)
    {}

    ~Impl()
    {
        request_shutdown();
        accept_thread.reset();
        command_thread.reset();
        std::unordered_map<std::uint64_t, std::shared_ptr<Client>> joined;
        {
            std::scoped_lock lock(clients_mutex);
            joined = std::move(clients);
        }
        joined.clear();
        remove_session_files();
    }

    void remove_session_files() noexcept
    {
        std::error_code ignored;
        if (owns_session_lock && !session_descriptor.empty()) {
            std::filesystem::remove(session_descriptor, ignored);
        }
        if (owns_session_lock && !session_endpoint.empty()) {
            std::filesystem::remove(session_endpoint, ignored);
        }
        if (lock_fd >= 0) {
            (void)::flock(lock_fd, LOCK_UN);
            (void)::close(lock_fd);
            lock_fd = -1;
        }
        owns_session_lock = false;
    }

    void publish_session_descriptor()
    {
        auto const temporary = session_directory
            / ("descriptor.json.tmp." + std::to_string(::getpid()));
        auto const descriptor = Json{
            {"protocolVersion", session_protocol_version},
            {"pid", ::getpid()},
            {"startupTimeUnixMs", unix_time_milliseconds()},
            {"projectRoot", workspace_root.generic_string()},
            {"endpoint", Json{
                {"type", "unix"},
                {"path", session_endpoint.generic_string()},
            }},
            {"localCapability", local_capability},
        }.dump(2) + "\n";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error(
                    "failed to create project session descriptor: "
                    + temporary.string());
            }
            output.write(
                descriptor.data(), static_cast<std::streamsize>(descriptor.size()));
            output.close();
            if (!output) {
                throw std::runtime_error(
                    "failed to write project session descriptor: "
                    + temporary.string());
            }
        }
        if (::chmod(temporary.c_str(), S_IRUSR | S_IWUSR) != 0) {
            std::filesystem::remove(temporary);
            throw std::runtime_error(
                "failed to protect project session descriptor: "
                + std::string(std::strerror(errno)));
        }
        std::error_code error;
        std::filesystem::rename(temporary, session_descriptor, error);
        if (error) {
            std::filesystem::remove(temporary);
            throw std::runtime_error(
                "failed to publish project session descriptor: "
                + error.message());
        }
    }

    void setup_local_endpoint()
    {
        session_directory = session_runtime_root()
            / project_path_hash(workspace_root);
        session_endpoint = session_directory / "server.sock";
        session_descriptor = session_directory / "descriptor.json";

        std::error_code error;
        std::filesystem::create_directories(session_directory, error);
        if (error) {
            throw std::runtime_error(
                "failed to create project session directory: " + error.message());
        }
        std::filesystem::permissions(
            session_directory,
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace,
            error);
        if (error) {
            throw std::runtime_error(
                "failed to protect project session directory: " + error.message());
        }

        auto const lock_path = session_directory / "lock";
        lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (lock_fd < 0) {
            throw std::runtime_error(
                "failed to open project session lock: "
                + std::string(std::strerror(errno)));
        }
        if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            auto const message = errno == EWOULDBLOCK
                ? "another server owns this project session"
                : "failed to lock project session: "
                    + std::string(std::strerror(errno));
            (void)::close(lock_fd);
            lock_fd = -1;
            throw std::runtime_error(message);
        }
        owns_session_lock = true;

        std::filesystem::remove(session_descriptor, error);
        error.clear();
        std::filesystem::remove(session_endpoint, error);

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        auto const endpoint = session_endpoint.string();
        if (endpoint.size() >= sizeof(address.sun_path)) {
            throw std::runtime_error(
                "project session socket path is too long: " + endpoint);
        }
        std::copy(endpoint.begin(), endpoint.end(), address.sun_path);
        address.sun_path[endpoint.size()] = '\0';

        listen_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd < 0) {
            throw std::runtime_error(
                "failed to create project session socket: "
                + std::string(std::strerror(errno)));
        }
        if (::bind(
                listen_fd,
                reinterpret_cast<sockaddr const*>(&address),
                sizeof(address)) != 0) {
            throw std::runtime_error(
                "failed to bind project session socket: "
                + std::string(std::strerror(errno)));
        }
        if (::chmod(session_endpoint.c_str(), S_IRUSR | S_IWUSR) != 0) {
            throw std::runtime_error(
                "failed to protect project session socket: "
                + std::string(std::strerror(errno)));
        }
        if (::listen(listen_fd, 32) != 0) {
            throw std::runtime_error(
                "failed to listen on project session socket: "
                + std::string(std::strerror(errno)));
        }
        local_capability = random_capability();
        publish_session_descriptor();
    }

    void schedule_client_cleanup(Client* client)
    {
        {
            std::scoped_lock lock(commands_mutex);
            if (shutting_down.load()) return;
            commands.push_back(Command{
                .disconnected_client_key = client->key,
            });
        }
        commands_ready.notify_one();
    }

    bool enqueue(
        std::shared_ptr<Client> const& client,
        std::string message,
        bool shutdown_after_write = false)
    {
        bool overflow = false;
        {
            std::scoped_lock lock(client->mutex);
            if (client->closing) return false;
            if (message.size() > max_client_outbound_bytes
                || client->outbound_bytes
                    > max_client_outbound_bytes - message.size()) {
                client->closing = true;
                overflow = true;
            } else {
                client->outbound_bytes += message.size();
                client->outbound.push_back(OutboundMessage{
                    .value = std::move(message),
                    .shutdown_after_write = shutdown_after_write,
                });
            }
        }
        if (overflow) {
            ::shutdown(client->fd, SHUT_RDWR);
            client->queue_ready.notify_all();
            schedule_client_cleanup(client.get());
            return false;
        }
        client->queue_ready.notify_one();
        return true;
    }

    void disconnect(Client* client)
    {
        {
            std::scoped_lock lock(client->mutex);
            if (client->closing) return;
            client->closing = true;
        }
        ::shutdown(client->fd, SHUT_RDWR);
        client->queue_ready.notify_all();
        schedule_client_cleanup(client);
    }

    void writer_loop(Client* client)
    {
        for (;;) {
            OutboundMessage message;
            {
                std::unique_lock lock(client->mutex);
                client->queue_ready.wait(lock, [&] {
                    return client->closing || !client->outbound.empty();
                });
                if (client->outbound.empty()) return;
                message = std::move(client->outbound.front());
                client->outbound.pop_front();
                client->outbound_bytes -= message.value.size();
            }
            if (!write_all(client->fd, message.value)) {
                disconnect(client);
                return;
            }
            if (message.shutdown_after_write) {
                request_shutdown();
                return;
            }
        }
    }

    void reader_loop(Client* client)
    {
        std::string buffer;
        std::array<char, 4096> input{};
        for (;;) {
            auto const count = ::read(client->fd, input.data(), input.size());
            if (count <= 0) {
                if (count < 0 && errno == EINTR) continue;
                disconnect(client);
                return;
            }
            buffer.append(input.data(), static_cast<std::size_t>(count));
            for (;;) {
                auto const newline = buffer.find('\n');
                if (newline == std::string::npos) break;
                auto line = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                if (line.empty()) continue;
                {
                    std::scoped_lock lock(commands_mutex);
                    if (shutting_down.load()) return;
                    commands.push_back(Command{
                        client->shared_from_this(), std::move(line)});
                }
                commands_ready.notify_one();
            }
        }
    }

    void accept_loop()
    {
        while (!shutting_down.load()) {
            auto const fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EINTR) continue;
                if (shutting_down.load() || errno == EBADF || errno == EINVAL) {
                    return;
                }
                continue;
            }
            try {
                attach_client(fd, false);
            } catch (...) {
                (void)::close(fd);
                if (shutting_down.load()) return;
            }
        }
    }

    void attach_client(int fd, bool trusted)
    {
        if (fd < 0) throw std::invalid_argument("client fd must be valid");
        if (!started || shutting_down.load()) {
            throw std::logic_error("SocketRpcServer is not accepting clients");
        }
        auto client = std::make_shared<Client>();
        client->fd = fd;
        client->authenticated = trusted;
        client->subscribed = trusted;
        client->identity = trusted ? "inherited-local-client" : std::string{};
        if (trusted) {
            client->capabilities = {
                "project.read", "project.write",
                "audio.control", "server.shutdown",
            };
        }
        {
            std::scoped_lock lock(clients_mutex);
            client->key = next_client_key++;
            clients.emplace(client->key, client);
        }
        client->writer.emplace(
            [this, value = client.get()](std::stop_token) {
                writer_loop(value);
            });
        client->reader.emplace(
            [this, value = client.get()](std::stop_token) {
                reader_loop(value);
            });
        (void)enqueue(client, jsonrpc_notification("server.ready", Json{
            {"protocolVersion", session_protocol_version},
            {"sessionRevision", current_revision()},
            {"authenticationRequired", !trusted},
        }));
    }

    void begin_deferred_notifications()
    {
        std::scoped_lock lock(deferred_mutex);
        deferring_notifications = true;
        deferring_thread = std::this_thread::get_id();
        deferred_notifications.clear();
    }

    std::vector<PendingNotification> finish_deferred_notifications()
    {
        std::scoped_lock lock(deferred_mutex);
        deferring_notifications = false;
        deferring_thread = {};
        return std::move(deferred_notifications);
    }

    std::uint64_t current_revision() const
    {
        std::scoped_lock lock(session_mutex);
        return revision;
    }

    std::uint64_t advance_revision()
    {
        std::scoped_lock lock(session_mutex);
        if (revision == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("project session revision exhausted");
        }
        return ++revision;
    }

    static bool topic_matches(Client const& client, std::string const& method)
    {
        return client.topics.empty() || client.topics.contains(method);
    }

    void publish_at_revision(
        std::string method, Json params, std::uint64_t notification_revision)
    {
        params["revision"] = notification_revision;
        auto const message = jsonrpc_notification(method, std::move(params));
        {
            std::scoped_lock lock(session_mutex);
            replay.push_back(ReplayEntry{
                notification_revision, method, message});
            replay_bytes += message.size();
            while (replay.size() > max_replay_entries
                   || replay_bytes > max_replay_bytes) {
                auto const evicted_revision = replay.front().revision;
                do {
                    replay_bytes -= replay.front().message.size();
                    replay.pop_front();
                } while (!replay.empty()
                         && replay.front().revision == evicted_revision);
                replay_floor = std::max(replay_floor, evicted_revision);
            }
        }

        std::vector<std::shared_ptr<Client>> targets;
        {
            std::scoped_lock lock(clients_mutex);
            for (auto const& [key, client] : clients) {
                (void)key;
                std::scoped_lock client_lock(client->mutex);
                if (!client->closing && client->authenticated
                    && client->subscribed && topic_matches(*client, method)) {
                    targets.push_back(client);
                }
            }
        }
        for (auto const& client : targets) (void)enqueue(client, message);
    }

    void publish(std::string method, Json params)
    {
        {
            std::scoped_lock lock(deferred_mutex);
            if (deferring_notifications
                && deferring_thread == std::this_thread::get_id()) {
                deferred_notifications.push_back(
                    PendingNotification{std::move(method), std::move(params)});
                return;
            }
        }
        {
            std::scoped_lock lock(commands_mutex);
            if (shutting_down.load()) return;
            commands.push_back(Command{
                .notification_method = std::move(method),
                .notification_params = std::move(params),
            });
        }
        commands_ready.notify_one();
    }

    void authorize(
        std::shared_ptr<Client> const& client,
        std::string const& required) const
    {
        if (required.empty()) return;
        std::scoped_lock lock(client->mutex);
        if (!client->authenticated) {
            throw std::runtime_error("session client is not authenticated");
        }
        if (!client->capabilities.contains(required)) {
            throw std::runtime_error(
                "session client lacks capability: " + required);
        }
    }

    std::string hello(
        std::shared_ptr<Client> const& client,
        int id,
        SessionHelloRequest const& request)
    {
        if (request.client_id.empty()) {
            throw std::runtime_error("session clientId must not be empty");
        }
        {
            std::scoped_lock lock(client->mutex);
            if (!client->authenticated
                && (local_capability.empty()
                    || request.local_capability != local_capability)) {
                throw std::runtime_error(
                    "session local capability is invalid");
            }
            client->authenticated = true;
            client->identity = request.client_id;
            client->capabilities = {
                "project.read", "project.write",
                "audio.control", "server.shutdown",
            };
        }
        return jsonrpc_result(id, Json{
            {"protocolVersion", session_protocol_version},
            {"clientId", request.client_id},
            {"capabilities", Json::array({
                "project.read", "project.write",
                "audio.control", "server.shutdown",
            })},
        });
    }

    std::string subscribe(
        std::shared_ptr<Client> const& client,
        int id,
        SessionSubscribeRequest const& request,
        std::vector<std::string>& after_response)
    {
        std::uint64_t current = 0;
        std::uint64_t oldest = 0;
        bool snapshot_required = false;
        {
            std::scoped_lock session_lock(session_mutex);
            current = revision;
            oldest = replay_floor;
            snapshot_required = request.after_revision > revision
                || request.after_revision < replay_floor;
            if (!snapshot_required) {
                std::scoped_lock client_lock(client->mutex);
                client->topics.clear();
                client->topics.insert(
                    request.topics.begin(), request.topics.end());
                client->subscribed = true;
                for (auto const& entry : replay) {
                    if (entry.revision > request.after_revision
                        && topic_matches(*client, entry.method)) {
                        after_response.push_back(entry.message);
                    }
                }
            }
        }
        return jsonrpc_result(id, Json{
            {"currentRevision", current},
            {"oldestAvailableRevision", oldest},
            {"snapshotRequired", snapshot_required},
            {"replayedNotificationCount", after_response.size()},
        });
    }

    void process_command(Command command)
    {
        int request_id = 0;
        std::string response;
        std::vector<std::string> after_response;
        SocketRpcRouteResult route;
        bool route_known = false;
        begin_deferred_notifications();
        try {
            auto const request = parse_socket_rpc_request(command.line);
            request_id = request.request_id;
            route = routes.dispatch(request, SocketRpcRouteContext{
                .authorize = [this, client = command.client](
                    std::string const& required) {
                    authorize(client, required);
                },
                .before_invoke = [this, &request](bool mutates) {
                    if (request.expected_revision && !mutates) {
                        throw std::runtime_error(
                            "expectedRevision is only valid for mutation routes");
                    }
                    auto const current = current_revision();
                    if (request.expected_revision
                        && *request.expected_revision != current) {
                        throw std::runtime_error(
                            "stale project session revision: expected "
                            + std::to_string(*request.expected_revision)
                            + ", current " + std::to_string(current));
                    }
                },
                .hello = [this, client = command.client](
                    int id, SessionHelloRequest const& request) {
                    return hello(client, id, request);
                },
                .subscribe = [this, client = command.client, &after_response](
                    int id, SessionSubscribeRequest const& request) {
                    return subscribe(client, id, request, after_response);
                },
            });
            route_known = true;
            response = std::move(route.response);
        } catch (std::exception const& error) {
            response = jsonrpc_error(request_id, -32000, error.what());
        }
        auto deferred = finish_deferred_notifications();
        auto const succeeded = response_succeeded(response);
        auto response_revision = current_revision();
        if (route_known && route.mutates_session && succeeded) {
            response_revision = advance_revision();
        }
        if (!enqueue(
                command.client,
                stamp_response(response, response_revision),
                route_known && route.shutdown_after_response && succeeded)) {
            return;
        }
        for (auto& message : after_response) {
            if (!enqueue(command.client, std::move(message))) break;
        }
        if (route_known && route.mutates_session && succeeded) {
            if (deferred.empty()) {
                publish_at_revision(
                    "session.revisionChanged", Json::object(), response_revision);
            } else {
                for (auto& notification : deferred) {
                    publish_at_revision(
                        std::move(notification.method),
                        std::move(notification.params),
                        response_revision);
                }
            }
        } else {
            for (auto& notification : deferred) {
                publish(
                    std::move(notification.method),
                    std::move(notification.params));
            }
        }
    }

    void command_loop()
    {
        for (;;) {
            Command command;
            {
                std::unique_lock lock(commands_mutex);
                commands_ready.wait(lock, [&] {
                    return shutting_down.load() || !commands.empty();
                });
                if (commands.empty()) return;
                command = std::move(commands.front());
                commands.pop_front();
            }
            if (command.is_client_cleanup()) {
                std::shared_ptr<Client> retired;
                {
                    std::scoped_lock lock(clients_mutex);
                    auto const position = clients.find(
                        command.disconnected_client_key);
                    if (position != clients.end()) {
                        retired = std::move(position->second);
                        clients.erase(position);
                    }
                }
                retired.reset();
            } else if (command.is_notification()) {
                publish_at_revision(
                    std::move(command.notification_method),
                    std::move(command.notification_params),
                    advance_revision());
            } else {
                process_command(std::move(command));
            }
        }
    }

    void start()
    {
        {
            std::scoped_lock lock(lifecycle_mutex);
            if (started) throw std::logic_error("SocketRpcServer already started");
            started = true;
        }
        try {
            if (initial_rpc_fd < 0) setup_local_endpoint();
            command_thread.emplace(
                [this](std::stop_token) { command_loop(); });
            if (initial_rpc_fd >= 0) {
                auto const fd = std::exchange(initial_rpc_fd, -1);
                attach_client(fd, true);
            } else {
                accept_thread.emplace(
                    [this](std::stop_token) { accept_loop(); });
            }
        } catch (...) {
            request_shutdown();
            throw;
        }
        {
            std::scoped_lock lock(lifecycle_mutex);
            ready = true;
        }
        ready_cv.notify_all();
    }

    void request_shutdown()
    {
        if (shutting_down.exchange(true)) return;
        if (listen_fd >= 0) {
            (void)::shutdown(listen_fd, SHUT_RDWR);
            (void)::close(listen_fd);
            listen_fd = -1;
        }
        std::vector<std::shared_ptr<Client>> current;
        {
            std::scoped_lock lock(clients_mutex);
            for (auto const& [key, client] : clients) {
                (void)key;
                current.push_back(client);
            }
        }
        for (auto const& client : current) disconnect(client.get());
        {
            std::scoped_lock lock(commands_mutex);
            commands.clear();
        }
        commands_ready.notify_all();
        if (command_thread) command_thread->request_stop();
        {
            std::scoped_lock lock(lifecycle_mutex);
            stopped = true;
        }
        ready_cv.notify_all();
        stopped_cv.notify_all();
    }
};

SocketRpcServer::SocketRpcServer(
    std::filesystem::path workspace_root,
    int initial_rpc_fd)
    : impl_(std::make_unique<Impl>(
          std::move(workspace_root), initial_rpc_fd))
{}

SocketRpcServer::~SocketRpcServer() = default;

void SocketRpcServer::start() { impl_->start(); }

void SocketRpcServer::attach_client(int connected_fd, bool trusted)
{
    impl_->attach_client(connected_fd, trusted);
}

bool SocketRpcServer::wait_until_ready(std::chrono::milliseconds timeout) const
{
    std::unique_lock lock(impl_->lifecycle_mutex);
    return impl_->ready_cv.wait_for(lock, timeout, [&] {
        return impl_->ready || impl_->stopped;
    }) && impl_->ready;
}

void SocketRpcServer::wait()
{
    {
        std::unique_lock lock(impl_->lifecycle_mutex);
        impl_->stopped_cv.wait(lock, [&] { return impl_->stopped; });
    }
    impl_->accept_thread.reset();
    impl_->command_thread.reset();
}

void SocketRpcServer::request_shutdown() { impl_->request_shutdown(); }

std::uint64_t SocketRpcServer::revision() const noexcept
{
    return impl_->current_revision();
}

std::size_t SocketRpcServer::client_count() const
{
    std::scoped_lock lock(impl_->clients_mutex);
    return std::ranges::count_if(
        impl_->clients,
        [](auto const& entry) {
            std::scoped_lock client_lock(entry.second->mutex);
            return !entry.second->closing;
        });
}

std::optional<std::filesystem::path> SocketRpcServer::endpoint() const
{
    if (impl_->session_endpoint.empty()) return std::nullopt;
    return impl_->session_endpoint;
}

void SocketRpcServer::send_server_message(
    SocketRpcServerMessage const& notification)
{
    impl_->publish("server.message", server_message_json(notification));
}

void SocketRpcServer::send_server_status(
    SocketRpcServerStatus const& notification)
{
    impl_->publish("server.status", server_status_json(notification));
}

void SocketRpcServer::send_node_instances_updated(
    std::vector<IvModuleInstanceInfo> const& instances)
{
    impl_->publish(
        "nodeInstances.updated",
        Json{{"instances", iv_module_instances_json(instances)}});
}

void SocketRpcServer::send_iv_package_definitions_updated()
{
    impl_->publish("ivPackages.updated", Json::object());
}

void SocketRpcServer::send_virtual_nodes_updated(
    ProjectVirtualNodesNotification const& notification)
{
    impl_->publish(
        "graph.nodesUpdated", virtual_nodes_updated_json(notification));
}

void SocketRpcServer::handle_project_notification(
    ProjectNotification const& notification)
{
    std::visit([this](auto const& payload) {
        using Payload = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::same_as<Payload, ProjectMessageNotification>) {
            send_server_message(payload);
        } else if constexpr (std::same_as<Payload, ProjectStatusNotification>) {
            send_server_status(payload);
        } else if constexpr (
            std::same_as<Payload, ProjectVirtualNodesNotification>) {
            send_virtual_nodes_updated(payload);
        }
    }, notification);
}

void SocketRpcServer::handle_iv_package_catalog_changed(
    IvPackageCatalogChanged const&)
{
    send_iv_package_definitions_updated();
}

void SocketRpcServer::handle_iv_module_instances_list_changed(
    std::vector<IvModuleInstanceInfo> const& instances)
{
    send_node_instances_updated(instances);
}

void SocketRpcServer::handle_iv_module_source_introspection_nodes_updated(
    ProjectVirtualNodesNotification const& notification)
{
    send_virtual_nodes_updated(notification);
}
} // namespace iv
