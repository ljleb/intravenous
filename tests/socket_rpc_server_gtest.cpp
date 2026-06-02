#include "module_test_utils.h"

#include <intravenous/runtime/socket_rpc_server.h>

#include <gtest/gtest.h>

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
#include <unistd.h>
#include <vector>

namespace {
    using namespace std::chrono_literals;

    constexpr auto socket_rpc_response_timeout = 10s;

    struct SocketRpcTestState {
        iv::SocketRpcServer *current_server = nullptr;

        std::vector<iv::GraphQueryBySpansRequest> graph_query_requests;
        iv::ProjectQueryResult graph_query_result;
        bool graph_query_should_fail = false;
        int graph_query_fail_code = -32000;
        std::string graph_query_fail_message;
        std::optional<iv::LaneViewResult> deferred_lane_view_notification;

        std::vector<iv::LaneViewRequest> open_lane_view_requests;
        std::vector<iv::LaneViewRequest> update_lane_view_requests;
        iv::LaneViewResult open_lane_view_result;
        iv::LaneViewResult update_lane_view_result;

        std::vector<std::string> close_lane_view_requests;
        bool close_lane_view_should_fail = false;
        int close_lane_view_fail_code = -32000;
        std::string close_lane_view_fail_message;

        int shutdown_hits = 0;

        void reset()
        {
            current_server = nullptr;
            graph_query_requests.clear();
            graph_query_result = {};
            graph_query_should_fail = false;
            graph_query_fail_code = -32000;
            graph_query_fail_message.clear();
            deferred_lane_view_notification.reset();
            open_lane_view_requests.clear();
            update_lane_view_requests.clear();
            open_lane_view_result = {};
            update_lane_view_result = {};
            close_lane_view_requests.clear();
            close_lane_view_should_fail = false;
            close_lane_view_fail_code = -32000;
            close_lane_view_fail_message.clear();
            shutdown_hits = 0;
        }
    };

    SocketRpcTestState socket_rpc_test_state;

    void handle_test_graph_query_by_spans(
        iv::GraphQueryBySpansRequest const &request,
        iv::SocketRpcGraphQueryResultBuilder &builder)
    {
        socket_rpc_test_state.graph_query_requests.push_back(request);
        if (socket_rpc_test_state.deferred_lane_view_notification.has_value() &&
            socket_rpc_test_state.current_server != nullptr) {
            socket_rpc_test_state.current_server->send_lane_view_updated(
                *socket_rpc_test_state.deferred_lane_view_notification);
        }
        if (socket_rpc_test_state.graph_query_should_fail) {
            builder.fail(
                socket_rpc_test_state.graph_query_fail_code,
                socket_rpc_test_state.graph_query_fail_message);
            return;
        }
        builder.succeed(socket_rpc_test_state.graph_query_result);
    }

    void handle_test_open_lane_view(
        iv::LaneViewRequest const &request,
        iv::SocketRpcLaneViewResultBuilder &builder)
    {
        socket_rpc_test_state.open_lane_view_requests.push_back(request);
        builder.succeed(socket_rpc_test_state.open_lane_view_result);
    }

    void handle_test_update_lane_view(
        iv::LaneViewRequest const &request,
        iv::SocketRpcLaneViewResultBuilder &builder)
    {
        socket_rpc_test_state.update_lane_view_requests.push_back(request);
        builder.succeed(socket_rpc_test_state.update_lane_view_result);
    }

    void handle_test_close_lane_view(
        std::string const &view_id,
        iv::SocketRpcAckResponseBuilder &builder)
    {
        socket_rpc_test_state.close_lane_view_requests.push_back(view_id);
        if (socket_rpc_test_state.close_lane_view_should_fail) {
            builder.fail(
                socket_rpc_test_state.close_lane_view_fail_code,
                socket_rpc_test_state.close_lane_view_fail_message);
        }
    }

    void handle_test_server_shutdown(
        iv::ServerShutdownRequest const &,
        iv::SocketRpcAckResponseBuilder &)
    {
        ++socket_rpc_test_state.shutdown_hits;
    }

    IV_SUBSCRIBE_LINKER_EVENT(
        iv::SocketRpcGraphQueryBySpansEvent,
        iv_socket_rpc_graph_query_by_spans_event,
        handle_test_graph_query_by_spans)
    IV_SUBSCRIBE_LINKER_EVENT(
        iv::SocketRpcOpenLaneViewEvent,
        iv_socket_rpc_open_lane_view_event,
        handle_test_open_lane_view)
    IV_SUBSCRIBE_LINKER_EVENT(
        iv::SocketRpcUpdateLaneViewEvent,
        iv_socket_rpc_update_lane_view_event,
        handle_test_update_lane_view)
    IV_SUBSCRIBE_LINKER_EVENT(
        iv::SocketRpcCloseLaneViewEvent,
        iv_socket_rpc_close_lane_view_event,
        handle_test_close_lane_view)
    IV_SUBSCRIBE_LINKER_EVENT(
        iv::SocketRpcServerShutdownEvent,
        iv_socket_rpc_server_shutdown_event,
        handle_test_server_shutdown)

    std::filesystem::path make_server_workspace()
    {
        auto const workspace = iv::test::fresh_module_fixture_workspace("socket_rpc_server");
        std::ofstream marker(workspace / ".intravenous", std::ios::binary | std::ios::trunc);
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
        std::string response_buffer;

        explicit SocketRpcHarness(std::filesystem::path const& workspace)
            : fds(make_socket_pair()),
              server(workspace, fds[1])
        {
            socket_rpc_test_state.current_server = &server;
            server.start();
            if (!server.wait_until_ready(5s)) {
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

        std::string read_line(std::chrono::milliseconds timeout = 5s)
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
    socket_rpc_test_state.graph_query_result.nodes.push_back(iv::LogicalNodeInfo {
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
    socket_rpc_test_state.open_lane_view_result.view_id = "open-view";
    socket_rpc_test_state.open_lane_view_result.lanes.start_index = 1;
    socket_rpc_test_state.update_lane_view_result.view_id = "update-view";
    socket_rpc_test_state.update_lane_view_result.lanes.start_index = 3;
    auto harness = SocketRpcHarness(make_server_workspace());

    ASSERT_FALSE(harness.read_line().empty());

    harness.write_request(
        R"({"jsonrpc":"2.0","id":3,"method":"timeline.openLaneView","params":{"viewId":"view-a","filter":{"kind":"graphInputs"},"startIndex":1,"visibleLaneCount":2}})"
        "\n");
    auto const open_response = harness.read_response(3);
    EXPECT_TRUE(open_response.contains(R"("viewId":"open-view")")) << open_response;

    harness.write_request(
        R"({"jsonrpc":"2.0","id":4,"method":"timeline.updateLaneView","params":{"viewId":"view-b","filter":{"kind":"graphInputs"},"startIndex":3,"visibleLaneCount":4}})"
        "\n");
    auto const update_response = harness.read_response(4);
    EXPECT_TRUE(update_response.contains(R"("viewId":"update-view")")) << update_response;

    ASSERT_EQ(socket_rpc_test_state.open_lane_view_requests.size(), 1u);
    EXPECT_EQ(socket_rpc_test_state.open_lane_view_requests.front().view_id, "view-a");
    EXPECT_EQ(socket_rpc_test_state.open_lane_view_requests.front().start_index, 1u);

    ASSERT_EQ(socket_rpc_test_state.update_lane_view_requests.size(), 1u);
    EXPECT_EQ(socket_rpc_test_state.update_lane_view_requests.front().view_id, "view-b");
    EXPECT_EQ(socket_rpc_test_state.update_lane_view_requests.front().start_index, 3u);
}

TEST(SocketRpcServer, AckSubscriberCanFailAndShutdownEventIsInvoked)
{
    socket_rpc_test_state.reset();
    socket_rpc_test_state.close_lane_view_should_fail = true;
    socket_rpc_test_state.close_lane_view_fail_code = -32077;
    socket_rpc_test_state.close_lane_view_fail_message = "close failed";
    auto harness = SocketRpcHarness(make_server_workspace());

    ASSERT_FALSE(harness.read_line().empty());

    harness.write_request(
        R"({"jsonrpc":"2.0","id":5,"method":"timeline.closeLaneView","params":{"viewId":"view-z"}})"
        "\n");
    auto const close_response = harness.read_response(5);
    EXPECT_TRUE(close_response.contains(R"("code":-32077)")) << close_response;
    EXPECT_TRUE(close_response.contains(R"("message":"close failed")")) << close_response;
    ASSERT_EQ(socket_rpc_test_state.close_lane_view_requests.size(), 1u);
    EXPECT_EQ(socket_rpc_test_state.close_lane_view_requests.front(), "view-z");

    harness.write_request(
        R"({"jsonrpc":"2.0","id":6,"method":"server.shutdown","params":{}})"
        "\n");
    auto const shutdown_response = harness.read_response(6);
    EXPECT_TRUE(shutdown_response.contains(R"("ok":true)")) << shutdown_response;
    EXPECT_EQ(socket_rpc_test_state.shutdown_hits, 1);
}

TEST(SocketRpcServer, DefersLaneViewNotificationsUntilAfterResponse)
{
    socket_rpc_test_state.reset();
    socket_rpc_test_state.graph_query_result.nodes.push_back(iv::LogicalNodeInfo {
        .id = "node-2",
        .kind = "DeferredNode",
    });
    socket_rpc_test_state.deferred_lane_view_notification = iv::LaneViewResult {
        .view_id = "deferred-view",
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
