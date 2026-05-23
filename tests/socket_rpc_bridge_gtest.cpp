#include "module_test_utils.h"

#include "runtime/graph_input_lanes.h"
#include "runtime/graph_input_lanes_lane_views_bridge.h"
#include "runtime/graph_input_lanes_timeline_bridge.h"
#include "runtime/iv_module_definitions.h"
#include "runtime/iv_module_definitions_iv_module_instances_bridge.h"
#include "runtime/iv_module_definitions_project_introspection_bridge.h"
#include "runtime/iv_module_instances.h"
#include "runtime/iv_module_instances_graph_input_lanes_bridge.h"
#include "runtime/lane_views.h"
#include "runtime/lane_views_timeline_bridge.h"
#include "runtime/project_introspection.h"
#include "runtime/project_introspection_graph_input_lanes_bridge.h"
#include "runtime/socket_rpc_lane_views_bridge.h"
#include "runtime/socket_rpc_notification_bridge.h"
#include "runtime/socket_rpc_project_introspection_bridge.h"
#include "runtime/socket_rpc_server.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
    using namespace std::chrono_literals;
    using Json = nlohmann::ordered_json;
    using namespace iv;

    class BackgroundTestAudioDevice {
        iv::RenderConfig _config {};
        std::vector<iv::Sample> _buffer;

    public:
        BackgroundTestAudioDevice()
        {
            _config.max_block_frames = 256;
            _config.preferred_block_size = 64;
            _buffer.assign(_config.max_block_frames * _config.num_channels, 0.0f);
        }

        iv::RenderConfig const& config() const
        {
            return _config;
        }

        std::span<iv::Sample> wait_for_block_request()
        {
            std::this_thread::sleep_for(1ms);
            return std::span<iv::Sample>(
                _buffer.data(),
                _config.preferred_block_size * _config.num_channels);
        }

        void submit_response()
        {}
    };

    auto make_audio_device_factory()
    {
        return []() -> std::optional<iv::LogicalAudioDevice> {
            return iv::LogicalAudioDevice(BackgroundTestAudioDevice {});
        };
    }

    std::filesystem::path make_project_workspace()
    {
        auto const workspace = iv::test::fresh_test_workspace("socket_rpc_bridge");
        iv::test::copy_directory(iv::test::test_modules_root() / "local_cmake", workspace);
        std::ofstream marker(workspace / ".intravenous", std::ios::binary | std::ios::trunc);
        EXPECT_TRUE(static_cast<bool>(marker));
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

    std::array<int, 2> make_socket_pair()
    {
        std::array<int, 2> fds { -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) != 0) {
            throw std::runtime_error(
                std::string("socketpair failed: ") + std::strerror(errno));
        }
        return fds;
    }

    class NotificationServerHarness {
        std::array<int, 2> fds;

    public:
        iv::SocketRpcServer server;
        std::string response_buffer;

        explicit NotificationServerHarness(std::filesystem::path const& workspace)
            : fds(make_socket_pair()),
              server(workspace, fds[1])
        {
            server.start();
            if (!server.wait_until_ready(5s)) {
                throw std::runtime_error("SocketRpcServer did not become ready");
            }
            auto const ready = read_line();
            if (ready.empty()) {
                throw std::runtime_error("SocketRpcServer did not emit ready");
            }
        }

        ~NotificationServerHarness()
        {
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

        std::string read_line(std::chrono::milliseconds timeout = 500ms)
        {
            return read_line_until(client_fd(), &response_buffer, timeout);
        }
    };

    Json parse_json_line(std::string_view line)
    {
        return Json::parse(line);
    }
}

TEST(SocketRpcRuntimeProjectBridge, UnboundQueryEventLeavesBuilderUnbuilt)
{
    auto const workspace = make_project_workspace();
    iv::SocketRpcGraphQueryResultBuilder builder;

    IV_INVOKE_LINKER_EVENT(
        iv_socket_rpc_graph_query_by_spans_event,
        iv::GraphQueryBySpansRequest {
            .file_path = workspace / "module.cpp",
            .ranges = {
                iv::SourceRange {
                    .start = { .line = 7, .column = 1 },
                    .end = { .line = 15, .column = 1 },
                },
            },
            .match_mode = iv::SourceRangeMatchMode::intersection,
        },
        builder);

    EXPECT_THROW((void)builder.build(1), std::runtime_error);
}

TEST(SocketRpcRuntimeProjectBridge, BoundQueryEventPopulatesBuilderFromService)
{
    auto const workspace = make_project_workspace();
    iv::Timeline timeline;
    iv::RuntimeIvModuleDefinitions iv_module_definitions(
        workspace,
        iv::test::repo_root(),
        std::vector<std::filesystem::path>{},
        make_audio_device_factory());
    iv::RuntimeIvModuleInstances iv_module_instances;
    iv::RuntimeGraphInputLanes graph_input_lanes;
    iv::RuntimeLaneViews lane_views;
    iv::RuntimeProjectIntrospection introspection;
    iv::bind_graph_input_lanes_timeline_bridge(timeline);
    iv::bind_graph_input_lanes_lane_views_bridge(graph_input_lanes);
    iv::bind_iv_module_definitions_iv_module_instances_bridge(iv_module_instances);
    iv::bind_iv_module_definitions_project_introspection_bridge(introspection);
    iv::bind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
    iv::bind_project_introspection_graph_input_lanes_bridge(graph_input_lanes);
    iv::bind_lane_views_timeline_bridge(lane_views);
    iv::bind_socket_rpc_lane_views_bridge(lane_views);
    (void)iv_module_definitions.initialize();
    introspection.initialize();
    iv::bind_socket_rpc_project_introspection_bridge(introspection, graph_input_lanes);

    iv::SocketRpcGraphQueryResultBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv_socket_rpc_graph_query_by_spans_event,
        iv::GraphQueryBySpansRequest {
            .file_path = workspace / "module.cpp",
            .ranges = {
                iv::SourceRange {
                    .start = { .line = 7, .column = 1 },
                    .end = { .line = 15, .column = 1 },
                },
            },
            .match_mode = iv::SourceRangeMatchMode::intersection,
        },
        builder);

    auto const response = parse_json_line(builder.build(2));
    iv::unbind_socket_rpc_project_introspection_bridge(introspection, graph_input_lanes);
    iv::unbind_socket_rpc_lane_views_bridge(lane_views);
    iv::unbind_lane_views_timeline_bridge(lane_views);
    iv::unbind_project_introspection_graph_input_lanes_bridge(graph_input_lanes);
    iv::unbind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
    iv::unbind_iv_module_definitions_project_introspection_bridge(introspection);
    iv::unbind_iv_module_definitions_iv_module_instances_bridge(iv_module_instances);
    iv::unbind_graph_input_lanes_lane_views_bridge(graph_input_lanes);
    iv::unbind_graph_input_lanes_timeline_bridge(timeline);
    iv_module_definitions.request_shutdown();

    EXPECT_EQ(response["id"], 2);
    ASSERT_TRUE(response["result"].contains("nodes"));
    EXPECT_FALSE(response["result"]["nodes"].empty());
}

TEST(SocketRpcNotificationBridge, UnboundServerDropsNotifications)
{
    iv::RuntimeProjectNotification notification = iv::RuntimeProjectMessageNotification {
        .level = "info",
        .message = "hello",
    };

    EXPECT_NO_THROW(IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_notification_event,
        notification));
}

TEST(SocketRpcNotificationBridge, BoundServerForwardsNotificationVariants)
{
    auto harness = NotificationServerHarness(make_project_workspace());
    iv::bind_socket_rpc_notification_bridge(harness.server);

    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_notification_event,
        iv::RuntimeProjectNotification(iv::RuntimeProjectMessageNotification {
            .level = "warning",
            .message = "bridge message",
        }));
    auto const message_line = harness.read_line();
    ASSERT_FALSE(message_line.empty());
    auto const message_json = parse_json_line(message_line);
    EXPECT_EQ(message_json["method"], "server.message");
    EXPECT_EQ(message_json["params"]["level"], "warning");
    EXPECT_EQ(message_json["params"]["message"], "bridge message");

    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_notification_event,
        iv::RuntimeProjectNotification(iv::RuntimeProjectStatusNotification {
            .level = "info",
            .code = "rebuildFinished",
            .message = "done",
        }));
    auto const status_line = harness.read_line();
    ASSERT_FALSE(status_line.empty());
    auto const status_json = parse_json_line(status_line);
    EXPECT_EQ(status_json["method"], "server.status");
    EXPECT_EQ(status_json["params"]["code"], "rebuildFinished");
    EXPECT_EQ(status_json["params"]["message"], "done");

    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_notification_event,
        iv::RuntimeProjectNotification(iv::RuntimeProjectLaneViewNotification {
            .lane_view = iv::LaneViewResult {
                .view_id = "view-1",
                .lanes = iv::LaneQueryResult {
                    .start_index = 0,
                    .visible_lane_count = 1,
                    .total_lane_count = 1,
                },
            },
        }));
    auto const lane_line = harness.read_line();
    ASSERT_FALSE(lane_line.empty());
    auto const lane_json = parse_json_line(lane_line);
    EXPECT_EQ(lane_json["method"], "timeline.laneViewUpdated");
    EXPECT_EQ(lane_json["params"]["viewId"], "view-1");

    iv::unbind_socket_rpc_notification_bridge(harness.server);
}
