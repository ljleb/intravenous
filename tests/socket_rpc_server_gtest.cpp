#include "module_test_utils.h"

#include <intravenous/bridge.h>
#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/lane_views.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_audio_device_lanes_bridge.h>
#include <intravenous/runtime/socket_rpc_lane_views_bridge.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <cstring>
#include <string_view>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {
    using namespace std::chrono_literals;
    using namespace iv;

    constexpr auto socket_rpc_startup_timeout = 30s;
    constexpr auto socket_rpc_response_timeout = 30s;

    iv::InternedString intern(std::string_view value)
    {
        return iv::InternedString::from_view(value);
    }

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

    iv::AudioDeviceLanesBackend make_audio_backend()
    {
        return iv::AudioDeviceLanesBackend{
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
        std::optional<iv::LaneViewResult> deferred_lane_view_notification;

        void reset()
        {
            current_server = nullptr;
            graph_query_requests.clear();
            graph_query_result = {};
            graph_query_should_fail = false;
            graph_query_fail_code = -32000;
            graph_query_fail_message.clear();
            deferred_lane_view_notification.reset();
        }
        void handle_graph_query_by_spans(
        iv::GraphQueryBySpansRequest const &request,
        iv::SocketRpcGraphQueryResultBuilder &builder)
        {
        graph_query_requests.push_back(request);
        if (deferred_lane_view_notification.has_value() && current_server != nullptr) {
            current_server->send_lane_view_updated(*deferred_lane_view_notification);
        }
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
        auto const workspace = iv::test::fresh_module_fixture_workspace("socket_rpc_server");
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

    class SocketRpcHarness {
        std::array<int, 2> fds;

    public:
        iv::SocketRpcServer server;
        iv::AudioDeviceLanes audio_device_lanes;
        socket_rpc_server_test_state_bridge::scope test_state_scope;
        iv::socket_rpc_audio_device_lanes_bridge::scope audio_device_lanes_scope;
        std::string response_buffer;

        explicit SocketRpcHarness(std::filesystem::path const& workspace)
            : fds(make_socket_pair()),
              server(workspace, fds[1]),
              audio_device_lanes(48000, 8, make_audio_backend()),
              test_state_scope(server, socket_rpc_test_state),
              audio_device_lanes_scope(server, audio_device_lanes)
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

TEST(SocketRpcServer, SendsReadyNotificationAndStopsOnDisconnect)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());

    auto const ready = harness.read_line();
    ASSERT_FALSE(ready.empty());
    EXPECT_TRUE(ready.contains(R"("method":"server.ready")")) << ready;

    harness.close_client_fd();
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

TEST(SocketRpcServer, DistinguishesOpenAndUpdateLaneViewEvents)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());
    iv::LaneViews lane_views;
    auto lane_views_scope = iv::socket_rpc_lane_views_bridge::bind(
        harness.server,
        lane_views);

    ASSERT_FALSE(harness.read_line().empty());

    harness.write_request(
        R"({"jsonrpc":"2.0","id":3,"method":"timeline.openLaneView","params":{"viewId":"view-a","filter":{"kind":"graphInputs"},"startIndex":1,"visibleLaneCount":2}})"
        "\n");
    auto const open_response = harness.read_response(3);
    EXPECT_TRUE(open_response.contains(R"("viewId":"view-a")")) << open_response;

    harness.write_request(
        R"({"jsonrpc":"2.0","id":4,"method":"timeline.updateLaneView","params":{"viewId":"view-b","filter":{"kind":"graphInputs"},"startIndex":3,"visibleLaneCount":4}})"
        "\n");
    auto const update_response = harness.read_response(4);
    EXPECT_TRUE(update_response.contains(R"("viewId":"view-b")")) << update_response;

    auto const active_requests = lane_views.active_view_requests();
    auto const open_request = std::ranges::find_if(active_requests, [](auto const &request) {
        return request.view_id.str() == "view-a";
    });
    ASSERT_NE(open_request, active_requests.end());
    EXPECT_EQ(open_request->start_index, 1u);

    auto const update_request = std::ranges::find_if(active_requests, [](auto const &request) {
        return request.view_id.str() == "view-b";
    });
    ASSERT_NE(update_request, active_requests.end());
    EXPECT_EQ(update_request->start_index, 3u);
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
    auto const snapshot = harness.audio_device_lanes.audio_devices_snapshot();
    EXPECT_EQ(snapshot.selected_output.device_id, std::optional<std::string>{"out-1"});
    EXPECT_FALSE(snapshot.selected_input.device_id.has_value());
}

TEST(SocketRpcServer, UnboundRequestReturnsTheOwningServiceError)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());

    ASSERT_FALSE(harness.read_line().empty());

    harness.write_request(
        R"({"jsonrpc":"2.0","id":10,"method":"playback.pause","params":{}})"
        "\n");
    auto const response = harness.read_response(10);
    EXPECT_TRUE(response.contains("timeline execution service is unavailable"))
        << response;
}

TEST(SocketRpcServer, ClosesLaneViewAndShutsDown)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());
    iv::LaneViews lane_views;
    auto lane_views_scope = iv::socket_rpc_lane_views_bridge::bind(
        harness.server,
        lane_views);

    ASSERT_FALSE(harness.read_line().empty());

    harness.write_request(
        R"({"jsonrpc":"2.0","id":5,"method":"timeline.openLaneView","params":{"viewId":"view-z","filter":{"kind":"graphInputs"}}})"
        "\n");
    ASSERT_FALSE(harness.read_response(5).empty());

    harness.write_request(
        R"({"jsonrpc":"2.0","id":6,"method":"timeline.closeLaneView","params":{"viewId":"view-z"}})"
        "\n");
    auto const close_response = harness.read_response(6);
    EXPECT_TRUE(close_response.contains(R"("ok":true)")) << close_response;
    EXPECT_TRUE(lane_views.active_view_requests().empty());

    harness.write_request(
        R"({"jsonrpc":"2.0","id":7,"method":"server.shutdown","params":{}})"
        "\n");
    auto const shutdown_response = harness.read_response(7);
    EXPECT_TRUE(shutdown_response.contains(R"("ok":true)")) << shutdown_response;
}

TEST(SocketRpcServer, DefersLaneViewNotificationsUntilAfterResponse)
{
    socket_rpc_test_state.reset();
    socket_rpc_test_state.graph_query_result.nodes.push_back(iv::VirtualNodeInfo {
        .id = "node-2",
        .kind = "DeferredNode",
    });
    socket_rpc_test_state.deferred_lane_view_notification = iv::LaneViewResult {
        .view_id = intern("deferred-view"),
        .lanes = iv::LaneQueryResult {
            .start_index = 0,
            .visible_lane_count = 1,
            .total_lane_count = 1,
        },
    };
    auto workspace = make_server_workspace();
    auto harness = SocketRpcHarness(workspace);

    ASSERT_FALSE(harness.read_line().empty());

    std::string const query_request =
        R"({"jsonrpc":"2.0","id":7,"method":"graph.queryBySpans","params":{"filePath":")" +
        (workspace / "module.cpp").generic_string() +
        R"(","ranges":[{"start":{"line":1,"column":1},"end":{"line":1,"column":1}}],"match":"intersection"}})" "\n";
    harness.write_request(query_request);

    auto const query_response = harness.read_response(7);
    ASSERT_FALSE(query_response.empty());
    EXPECT_TRUE(query_response.contains(R"("id":"node-2")")) << query_response;

    auto const deferred_notification = harness.read_line(5s);
    ASSERT_FALSE(deferred_notification.empty());
    EXPECT_TRUE(deferred_notification.contains(R"("method":"timeline.laneViewUpdated")")) << deferred_notification;
    EXPECT_TRUE(deferred_notification.contains(R"("viewId":"deferred-view")")) << deferred_notification;
}
