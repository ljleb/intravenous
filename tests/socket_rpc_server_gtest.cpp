#include "module_test_utils.h"

#include <intravenous/bridge.h>
#include <intravenous/runtime/system_audio_devices.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_system_audio_devices_bridge.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <cstring>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {
    using namespace std::chrono_literals;
    using namespace iv;

    constexpr auto socket_rpc_startup_timeout = 30s;
    constexpr auto socket_rpc_response_timeout = 30s;

    struct IdleFakeAudioInputDevice {
        iv::RenderConfig config_;

        explicit IdleFakeAudioInputDevice(iv::RenderConfig config_)
            : config_(std::move(config_))
        {
            iv::validate_render_config(config_);
        }

        iv::RenderConfig const &config() const { return config_; }

        iv::AudioInputBlock wait_for_captured_block()
        {
            throw std::logic_error("idle fake input device has no captured blocks");
        }

        void release_captured_block() {}
        void request_shutdown() {}
    };

    iv::SystemAudioDevicesBackend make_audio_backend()
    {
        return iv::SystemAudioDevicesBackend{
            .list_output_devices = [] {
                return std::vector<iv::AudioDeviceDescriptor>{
                    {.device_id = "default", .name = "System Default"},
                    {.device_id = "out-1", .name = "Output 1"},
                };
            },
            .list_input_devices = [] {
                return std::vector<iv::AudioDeviceDescriptor>{
                    {.device_id = "default", .name = "System Default"},
                    {.device_id = "in-1", .name = "Input 1"},
                };
            },
            .make_output_device = [](std::string const &, iv::RenderConfig const &config) {
                return iv::AudioOutputDevice(
                    std::in_place_type<iv::test::FakeAudioDevice>,
                    config);
            },
            .make_input_device = [](std::string const &, iv::RenderConfig const &config) {
                return iv::AudioInputDevice(
                    std::in_place_type<IdleFakeAudioInputDevice>,
                    config);
            },
        };
    }

    struct SocketRpcTestState {
        iv::SocketRpcServer *current_server = nullptr;

        std::vector<iv::GraphQueryBySpansRequest> graph_query_requests;
        iv::ProjectQueryResult graph_query_result;
        bool graph_query_should_fail = false;
        int graph_query_fail_code = -32000;
        std::string graph_query_fail_message;

        void reset()
        {
            current_server = nullptr;
            graph_query_requests.clear();
            graph_query_result = {};
            graph_query_should_fail = false;
            graph_query_fail_code = -32000;
            graph_query_fail_message.clear();
        }
        void handle_graph_query_by_spans(
        iv::GraphQueryBySpansRequest const &request,
        iv::SocketRpcGraphQueryResultBuilder &builder)
        {
        graph_query_requests.push_back(request);
        if (graph_query_should_fail) {
            builder.fail(
                graph_query_fail_code,
                graph_query_fail_message);
            return;
        }
        builder.succeed(graph_query_result);
        }

    };

    SocketRpcTestState socket_rpc_test_state;

    IV_DECLARE_BRIDGE(
        socket_rpc_server_test_state_bridge,
        iv::SocketRpcServer,
        SocketRpcTestState);
    IV_DEFINE_BRIDGE(socket_rpc_server_test_state_bridge)

    IV_SUBSCRIBE_LINKER_EVENT(
        socket_rpc_server_test_state_bridge,
        iv_socket_rpc_graph_query_by_spans_event,
        &SocketRpcTestState::handle_graph_query_by_spans)
    std::filesystem::path make_server_workspace()
    {
        auto const* info = ::testing::UnitTest::GetInstance()->current_test_info();
        auto const test_name = info
            ? std::string("socket_rpc_server_") + info->test_suite_name()
                + "_" + info->name()
            : std::string("socket_rpc_server");
        auto const workspace = iv::test::fresh_module_fixture_workspace(test_name);
        std::ofstream marker(workspace / "iv_project.jsonl", std::ios::binary | std::ios::trunc);
        EXPECT_TRUE(static_cast<bool>(marker));
        std::ofstream module_cpp(workspace / "module.cpp", std::ios::binary | std::ios::trunc);
        EXPECT_TRUE(static_cast<bool>(module_cpp));
        module_cpp << "// test module\n";
        return workspace;
    }

    std::string pop_line(std::string* buffer)
    {
        auto const newline = buffer->find('\n');
        if (newline == std::string::npos) {
            return {};
        }
        auto line = buffer->substr(0, newline);
        buffer->erase(0, newline + 1);
        return line;
    }

    std::string read_line_until(int fd, std::string* buffer, std::chrono::milliseconds timeout)
    {
        if (auto line = pop_line(buffer); !line.empty()) {
            return line;
        }

        auto const deadline = std::chrono::steady_clock::now() + timeout;
        std::array<char, 256> read_buffer {};

        while (std::chrono::steady_clock::now() < deadline) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(fd, &read_fds);

            auto const remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                break;
            }

            timeval tv {};
            tv.tv_sec = static_cast<long>(remaining.count() / 1000000);
            tv.tv_usec = static_cast<long>(remaining.count() % 1000000);

            int ready = ::select(fd + 1, &read_fds, nullptr, nullptr, &tv);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (ready == 0) {
                continue;
            }

            ssize_t count = ::read(fd, read_buffer.data(), read_buffer.size());
            if (count <= 0) {
                break;
            }

            buffer->append(read_buffer.data(), static_cast<size_t>(count));
            if (auto line = pop_line(buffer); !line.empty()) {
                return line;
            }
        }

        return {};
    }

    std::string read_response_for_id(
        int fd,
        std::string* buffer,
        int id,
        std::chrono::milliseconds timeout = socket_rpc_response_timeout)
    {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        std::string const marker = "\"id\":" + std::to_string(id);
        while (std::chrono::steady_clock::now() < deadline) {
            auto const line = read_line_until(fd, buffer, 500ms);
            if (line.empty()) {
                continue;
            }
            if (line.contains(marker)) {
                return line;
            }
        }
        return {};
    }

    std::array<int, 2> make_socket_pair()
    {
        std::array<int, 2> fds { -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) != 0) {
            throw std::runtime_error(
                std::string("socketpair failed: ") + std::strerror(errno));
        }
        return fds;
    }

    int connect_unix_socket(std::filesystem::path const& endpoint)
    {
        auto const fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            throw std::runtime_error(
                std::string("socket failed: ") + std::strerror(errno));
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        auto const value = endpoint.string();
        if (value.size() >= sizeof(address.sun_path)) {
            ::close(fd);
            throw std::runtime_error("test Unix socket path is too long");
        }
        std::copy(value.begin(), value.end(), address.sun_path);
        if (::connect(
                fd,
                reinterpret_cast<sockaddr const*>(&address),
                sizeof(address)) != 0) {
            auto const message = std::string(std::strerror(errno));
            ::close(fd);
            throw std::runtime_error("connect failed: " + message);
        }
        return fd;
    }

    class SocketRpcHarness {
        std::array<int, 2> fds;

    public:
        iv::SocketRpcServer server;
        iv::SystemAudioDevices system_audio_devices;
        socket_rpc_server_test_state_bridge::scope test_state_scope;
        iv::socket_rpc_system_audio_devices_bridge::scope system_audio_devices_scope;
        std::string response_buffer;

        explicit SocketRpcHarness(std::filesystem::path const& workspace)
            : fds(make_socket_pair()),
              server(workspace, fds[1]),
              system_audio_devices(48000, 8, make_audio_backend()),
              test_state_scope(server, socket_rpc_test_state),
              system_audio_devices_scope(server, system_audio_devices)
        {
            socket_rpc_test_state.current_server = &server;
            server.start();
            if (!server.wait_until_ready(socket_rpc_startup_timeout)) {
                throw std::runtime_error("SocketRpcServer did not become ready");
            }
        }

        ~SocketRpcHarness()
        {
            socket_rpc_test_state.current_server = nullptr;
            if (fds[0] >= 0) {
                ::close(fds[0]);
            }
            server.request_shutdown();
            server.wait();
        }

        int client_fd() const
        {
            return fds[0];
        }

        std::string read_line(std::chrono::milliseconds timeout = socket_rpc_startup_timeout)
        {
            return read_line_until(client_fd(), &response_buffer, timeout);
        }

        std::string read_response(
            int id,
            std::chrono::milliseconds timeout = socket_rpc_response_timeout)
        {
            return read_response_for_id(client_fd(), &response_buffer, id, timeout);
        }

        void write_request(std::string const& request) const
        {
            ASSERT_EQ(
                ::write(client_fd(), request.data(), request.size()),
                static_cast<ssize_t>(request.size()));
        }

        void close_client_fd()
        {
            if (fds[0] >= 0) {
                ::close(fds[0]);
                fds[0] = -1;
            }
        }
    };
}

TEST(SocketRpcServer, SendsReadyNotificationAndSurvivesClientDisconnect)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());

    auto const ready = harness.read_line();
    ASSERT_FALSE(ready.empty());
    EXPECT_TRUE(ready.contains(R"("method":"server.ready")")) << ready;

    harness.close_client_fd();
    auto const deadline = std::chrono::steady_clock::now() + 1s;
    while (harness.server.client_count() != 0
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_EQ(harness.server.client_count(), 0u);
    harness.server.request_shutdown();
    harness.server.wait();
}

TEST(SocketRpcServer, DispatchesQueryEventAndReturnsSubscriberResult)
{
    socket_rpc_test_state.reset();
    socket_rpc_test_state.graph_query_result.nodes.push_back(iv::VirtualNodeInfo {
        .id = "node-1",
        .kind = "TestNode",
        .source_identity = "src-1",
        .type_identity = "TestNode",
    });
    auto workspace = make_server_workspace();
    auto harness = SocketRpcHarness(workspace);

    ASSERT_FALSE(harness.read_line().empty());

    auto const module_path = (workspace / "module.cpp").generic_string();
    std::string const query_request =
        R"({"jsonrpc":"2.0","id":2,"method":"graph.queryBySpans","params":{"filePath":")" +
        module_path +
        R"(","ranges":[{"start":{"line":7,"column":1},"end":{"line":15,"column":1}}],"match":"intersection"}})" "\n";
    harness.write_request(query_request);

    auto const query_response = harness.read_response(2);
    ASSERT_FALSE(query_response.empty());
    EXPECT_TRUE(query_response.contains(R"("id":"node-1")")) << query_response;
    EXPECT_TRUE(query_response.contains(R"("kind":"TestNode")")) << query_response;

    ASSERT_EQ(socket_rpc_test_state.graph_query_requests.size(), 1u);
    auto const& captured = socket_rpc_test_state.graph_query_requests.front();
    EXPECT_EQ(captured.file_path, module_path);
    ASSERT_EQ(captured.ranges.size(), 1u);
    EXPECT_EQ(captured.ranges.front().start.line, 7u);
    EXPECT_EQ(captured.ranges.front().end.line, 15u);
    EXPECT_EQ(captured.match_mode, iv::SourceRangeMatchMode::intersection);
}



TEST(SocketRpcServer, DispatchesAudioDeviceGetAndSetRequests)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());

    ASSERT_FALSE(harness.read_line().empty());

    harness.write_request(
        R"({"jsonrpc":"2.0","id":8,"method":"audioDevices.get","params":{}})"
        "\n");
    auto const get_response = harness.read_response(8);
    EXPECT_TRUE(get_response.contains(R"("outputDevices")")) << get_response;
    EXPECT_TRUE(get_response.contains(R"("deviceId":"out-1")")) << get_response;

    harness.write_request(
        R"({"jsonrpc":"2.0","id":9,"method":"audioDevices.set","params":{"outputDeviceId":"out-1","inputDeviceId":null}})"
        "\n");
    auto const set_response = harness.read_response(9);
    EXPECT_TRUE(set_response.contains(R"("deviceId":"out-1")")) << set_response;
    EXPECT_TRUE(set_response.contains(R"("selectedInput")")) << set_response;
    auto const snapshot = harness.system_audio_devices.audio_devices_snapshot();
    EXPECT_EQ(snapshot.selected_output.device_id, std::optional<std::string>{"out-1"});
    EXPECT_FALSE(snapshot.selected_input.device_id.has_value());
}

TEST(SocketRpcServer, DeliversOneOrderedNotificationToEverySubscribedClient)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());
    ASSERT_FALSE(harness.read_line().empty());

    auto second = make_socket_pair();
    harness.server.attach_client(second[1], true);
    std::string second_buffer;
    ASSERT_FALSE(read_line_until(
        second[0], &second_buffer, socket_rpc_startup_timeout).empty());

    harness.server.send_server_message(iv::SocketRpcServerMessage{
        .level = "info",
        .message = "shared notification",
    });

    auto const first_message = harness.read_line();
    auto const second_message = read_line_until(
        second[0], &second_buffer, socket_rpc_response_timeout);
    ASSERT_FALSE(first_message.empty());
    ASSERT_FALSE(second_message.empty());
    auto const first_json = nlohmann::ordered_json::parse(first_message);
    auto const second_json = nlohmann::ordered_json::parse(second_message);
    EXPECT_EQ(first_json["method"], "server.message");
    EXPECT_EQ(second_json["method"], "server.message");
    EXPECT_EQ(first_json["params"]["revision"], 1);
    EXPECT_EQ(second_json["params"]["revision"], 1);

    ::close(second[0]);
}

TEST(SocketRpcServer, RejectsAStaleMutationAfterThePriorMutationCommits)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());
    ASSERT_FALSE(harness.read_line().empty());
    ASSERT_EQ(harness.server.revision(), 0u);

    harness.write_request(
        R"({"jsonrpc":"2.0","id":60,"method":"audioDevices.set","params":{"outputDeviceId":"out-1","inputDeviceId":null,"expectedRevision":0}})"
        "\n");
    auto const committed = nlohmann::ordered_json::parse(
        harness.read_response(60));
    EXPECT_EQ(committed["result"]["sessionRevision"], 1);

    harness.write_request(
        R"({"jsonrpc":"2.0","id":61,"method":"audioDevices.set","params":{"outputDeviceId":"default","inputDeviceId":null,"expectedRevision":0}})"
        "\n");
    auto const stale = nlohmann::ordered_json::parse(
        harness.read_response(61));
    EXPECT_TRUE(stale.contains("error"));
    EXPECT_EQ(stale["error"]["data"]["currentRevision"], 1);
    EXPECT_TRUE(
        stale["error"]["message"].get<std::string>().contains(
            "stale project session revision"));
    EXPECT_EQ(harness.server.revision(), 1u);
}

TEST(SocketRpcServer, ReplaysFromARevisionAndAppliesPerClientTopicFilters)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());
    ASSERT_FALSE(harness.read_line().empty());
    harness.server.send_server_message(iv::SocketRpcServerMessage{
        .level = "info",
        .message = "before attach",
    });
    ASSERT_TRUE(harness.read_line().contains("before attach"));

    auto second = make_socket_pair();
    harness.server.attach_client(second[1], true);
    std::string buffer;
    ASSERT_FALSE(read_line_until(
        second[0], &buffer, socket_rpc_startup_timeout).empty());
    auto const subscribe = std::string(
        R"({"jsonrpc":"2.0","id":65,"method":"session.subscribe","params":{"afterRevision":0,"topics":["server.message"]}})"
        "\n");
    ASSERT_EQ(
        ::write(second[0], subscribe.data(), subscribe.size()),
        static_cast<ssize_t>(subscribe.size()));
    auto const response = nlohmann::ordered_json::parse(read_response_for_id(
        second[0], &buffer, 65, socket_rpc_response_timeout));
    EXPECT_FALSE(response["result"]["snapshotRequired"]);
    EXPECT_EQ(response["result"]["replayedNotificationCount"], 1);
    auto const replayed = read_line_until(
        second[0], &buffer, socket_rpc_response_timeout);
    EXPECT_TRUE(replayed.contains("before attach"));

    harness.server.send_server_status(iv::SocketRpcServerStatus{
        .level = "info",
        .message = "filtered status",
    });
    harness.server.send_server_message(iv::SocketRpcServerMessage{
        .level = "info",
        .message = "selected message",
    });
    auto const selected = read_line_until(
        second[0], &buffer, socket_rpc_response_timeout);
    EXPECT_TRUE(selected.contains("selected message")) << selected;
    EXPECT_FALSE(selected.contains("filtered status")) << selected;
    ::close(second[0]);
}

TEST(SocketRpcServer, DisconnectsAClientWhoseOutboundQueueCannotAcceptAMessage)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());
    ASSERT_FALSE(harness.read_line().empty());

    harness.server.send_server_message(iv::SocketRpcServerMessage{
        .level = "info",
        .message = std::string(2 * 1024 * 1024, 'x'),
    });

    auto const deadline = std::chrono::steady_clock::now() + 2s;
    while (harness.server.client_count() != 0
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_EQ(harness.server.client_count(), 0u);

    auto replacement = make_socket_pair();
    harness.server.attach_client(replacement[1], true);
    std::string buffer;
    ASSERT_FALSE(read_line_until(
        replacement[0], &buffer, socket_rpc_startup_timeout).empty());
    auto const subscribe = std::string(
        R"({"jsonrpc":"2.0","id":66,"method":"session.subscribe","params":{"afterRevision":0,"topics":[]}})"
        "\n");
    ASSERT_EQ(
        ::write(replacement[0], subscribe.data(), subscribe.size()),
        static_cast<ssize_t>(subscribe.size()));
    auto const response = nlohmann::ordered_json::parse(read_response_for_id(
        replacement[0], &buffer, 66, socket_rpc_response_timeout));
    EXPECT_TRUE(response["result"]["snapshotRequired"]);
    EXPECT_EQ(response["result"]["oldestAvailableRevision"], 1);
    ::close(replacement[0]);
}

TEST(SocketRpcServer, PublishesProtectedLocalDescriptorAndElectsOneOwner)
{
    socket_rpc_test_state.reset();
    auto const workspace = make_server_workspace();
    std::filesystem::path descriptor_path;
    {
        iv::SocketRpcServer server(workspace);
        server.start();
        ASSERT_TRUE(server.wait_until_ready(socket_rpc_startup_timeout));
        auto const endpoint = server.endpoint();
        ASSERT_TRUE(endpoint.has_value());
        descriptor_path = endpoint->parent_path() / "descriptor.json";
        std::ifstream descriptor_input(descriptor_path);
        ASSERT_TRUE(static_cast<bool>(descriptor_input));
        auto const descriptor = nlohmann::ordered_json::parse(descriptor_input);
        EXPECT_EQ(descriptor["protocolVersion"], 1);
        EXPECT_EQ(
            descriptor["projectRoot"],
            std::filesystem::weakly_canonical(workspace).generic_string());
        ASSERT_TRUE(descriptor["localCapability"].is_string());
        EXPECT_FALSE(
            descriptor["localCapability"].get<std::string>().empty());

        iv::SocketRpcServer contender(workspace);
        EXPECT_THROW(contender.start(), std::runtime_error);
        EXPECT_TRUE(std::filesystem::exists(descriptor_path));

        auto const client_fd = connect_unix_socket(*endpoint);
        std::string buffer;
        auto const ready = read_line_until(
            client_fd, &buffer, socket_rpc_startup_timeout);
        EXPECT_TRUE(ready.contains(R"("authenticationRequired":true)"));

        auto const unauthenticated = std::string(
            R"({"jsonrpc":"2.0","id":63,"method":"audioDevices.get","params":{}})"
            "\n");
        ASSERT_EQ(
            ::write(
                client_fd, unauthenticated.data(), unauthenticated.size()),
            static_cast<ssize_t>(unauthenticated.size()));
        EXPECT_TRUE(read_response_for_id(
            client_fd, &buffer, 63, socket_rpc_response_timeout).contains(
                "session client is not authenticated"));

        auto const wrong_hello = std::string(
            R"({"jsonrpc":"2.0","id":64,"method":"session.hello","params":{"clientId":"test-client","localCapability":"wrong"}})"
            "\n");
        ASSERT_EQ(
            ::write(client_fd, wrong_hello.data(), wrong_hello.size()),
            static_cast<ssize_t>(wrong_hello.size()));
        EXPECT_TRUE(read_response_for_id(
            client_fd, &buffer, 64, socket_rpc_response_timeout).contains(
                "session local capability is invalid"));

        auto const hello = std::string(
            R"({"jsonrpc":"2.0","id":62,"method":"session.hello","params":{"clientId":"test-client","localCapability":")")
            + descriptor["localCapability"].get<std::string>()
            + R"("}})"
            + "\n";
        ASSERT_EQ(
            ::write(client_fd, hello.data(), hello.size()),
            static_cast<ssize_t>(hello.size()));
        auto const response = read_response_for_id(
            client_fd, &buffer, 62, socket_rpc_response_timeout);
        EXPECT_TRUE(response.contains(R"("clientId":"test-client")"));
        ::close(client_fd);

        server.request_shutdown();
        server.wait();
    }
    EXPECT_FALSE(std::filesystem::exists(descriptor_path));
}
