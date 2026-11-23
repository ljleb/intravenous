#include "module_test_utils.h"

#include <intravenous/bridge.h>
#include <intravenous/audio/system_audio_devices.h>
#include <intravenous/project/project_graph.h>
#include <intravenous/app/bridges/project_graph_system_audio_devices_bridge.h>
#include <intravenous/project/runtime_project_events.h>
#include <intravenous/app/bridges/socket_rpc_system_audio_devices_bridge.h>
#include <intravenous/rpc/socket_rpc_server.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
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

    // Stands in for IvModuleSourceIntrospection.
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

    std::string snapshot_configuration(std::size_t size)
    {
        // Four-byte UTF-8 characters exercise chunk boundaries in both JSON layers.
        std::string configuration;
        configuration.reserve(size);
        while (configuration.size() < size) configuration += "\xf0\x9f\x8e\xb5";
        return configuration;
    }

    std::string snapshot_lane_state()
    {
        std::string state = R"({"text":")";
        for (int index = 0; index < 63'000; ++index) state += "\\n";
        return state + R"("})";
    }

    // Persistence collection reaches NodeInstances in the real server.
    struct SessionProjectStateFake {
        std::atomic<unsigned> captures{0};

        void handle_get_session_project_state(
            iv::SessionSnapshotRequest const&,
            iv::SocketRpcSessionProjectStateResultBuilder& builder)
        {
            ++captures;
            iv::SocketRpcNodeInstancesResultBuilder instances;
            IV_INVOKE_LINKER_EVENT(iv_socket_rpc_get_node_instances_event, iv::GetNodeInstancesRequest{}, instances);
            std::vector<iv::ProjectCommand> commands{iv::ProjectCommand{
                .command = "nodeInstances.update",
                .args = nlohmann::ordered_json{
                    {"instance_id", "snapshot-instance"}, {"configuration_source", snapshot_configuration(540'000)}},
            }};
            auto const state = snapshot_lane_state();
            for (int lane = 0; lane < 7; ++lane) {
                commands.push_back({"projectPresentation.setLaneView", {{"index", lane}, {"state", state}}});
            }
            builder.succeed(std::move(commands));
        }
    };

    // Stands in for ProjectAutosave.
    struct AutosaveStateFake {
        void handle_get_autosave_state(
            iv::SessionSnapshotRequest const&,
            iv::SocketRpcAutosaveStateResultBuilder& builder)
        {
            builder.succeed(true);
        }
    };

    // Stands in for NodeInstances.
    struct NodeInstancesFake {
        std::atomic<bool> large_configurations{false};

        void handle_get_node_instances(
            iv::GetNodeInstancesRequest const&,
            iv::SocketRpcNodeInstancesResultBuilder& builder)
        {
            iv::IvModuleInstanceInfo instance{
                .instance_id = "snapshot-instance",
                .definition_id = "iv.test.snapshot.module",
                .display_name = "Snapshot instance",
                .realized = true,
            };
            if (!large_configurations.load()) {
                builder.succeed({std::move(instance)});
                return;
            }
            instance.configuration_source = snapshot_configuration(360'000);
            std::vector<iv::IvModuleInstanceInfo> instances;
            for (int index = 0; index < 3; ++index) {
                instance.instance_id = "snapshot-instance-" + std::to_string(index);
                instances.push_back(instance);
            }
            builder.succeed(std::move(instances));
        }
    };

    // Stands in for PackageDefinitions.
    struct PackageDefinitionsFake {
        void handle_get_package_definitions(
            iv::GetIvPackageDefinitionsRequest const&,
            iv::SocketRpcIvPackageDefinitionsResultBuilder& builder)
        {
            builder.succeed({iv::IvPackageInfo{
                .package_id = "iv.test.snapshot",
                .package_root = "/tmp/iv-test-snapshot",
            }});
        }
    };

    // Stands in for ProjectGraph.
    struct ProjectGraphFake {
        void handle_get_node_event_protocol(
            iv::GetNodeEventProtocolRequest const&,
            iv::SocketRpcNodeEventProtocolResultBuilder& builder)
        {
            builder.succeed(iv::NodeEventProtocolSnapshot{
                .project_graph_generation = 12,
                .definitions_generation = 7,
            });
        }

        // Accepts live values of the gesture named "drag" and closes it at end.
        std::atomic<bool> drag_open{false};
        void handle_live_manual_control_update(iv::LiveManualControlUpdateRequest& request)
        {
            if (request.interaction.interaction_id != "drag" || !drag_open.load()) return;
            request.applied = true;
            request.control = {.node = {.instance_id = "osc"}, .port_name = "frequency"};
        }
        void handle_begin_manual_control(
            iv::BeginManualControlInteractionRequest const&, iv::SocketRpcAckResponseBuilder& builder)
        {
            drag_open.store(true);
            builder.succeed();
        }
        void handle_end_manual_control(
            iv::EndManualControlInteractionRequest const&, iv::SocketRpcAckResponseBuilder& builder)
        {
            drag_open.store(false);
            builder.succeed();
        }
        // A closed client's open gesture commits as one durable change.
        void handle_client_disconnected(iv::SocketRpcClientDisconnectedRequest& request)
        {
            request.project_state_changed = drag_open.exchange(false);
        }

        // A mutation answers with the read-model changes it committed.
        void handle_delete_node_instance(
            iv::DeleteNodeInstanceRequest const&,
            iv::SocketRpcAckResponseBuilder& builder)
        {
            builder.set_publication(iv::ProjectGraphPublication{
                .instances = std::vector<iv::IvModuleInstanceInfo>{},
            });
            builder.succeed();
        }
    };


    IV_DECLARE_BRIDGE(
        socket_rpc_server_test_state_bridge,
        iv::SocketRpcServer,
        SocketRpcTestState);
    IV_DEFINE_BRIDGE(socket_rpc_server_test_state_bridge)

    IV_SUBSCRIBE_LINKER_EVENT(
        socket_rpc_server_test_state_bridge,
        iv_socket_rpc_graph_query_by_spans_event,
        &SocketRpcTestState::handle_graph_query_by_spans)
    IV_DECLARE_BRIDGE(
        socket_rpc_server_test_session_project_state_bridge,
        iv::SocketRpcServer,
        SessionProjectStateFake);
    IV_DEFINE_BRIDGE(socket_rpc_server_test_session_project_state_bridge)
    IV_SUBSCRIBE_LINKER_EVENT(
        socket_rpc_server_test_session_project_state_bridge,
        iv_socket_rpc_get_session_project_state_event,
        &SessionProjectStateFake::handle_get_session_project_state)
    IV_DECLARE_BRIDGE(
        socket_rpc_server_test_autosave_state_bridge,
        iv::SocketRpcServer,
        AutosaveStateFake);
    IV_DEFINE_BRIDGE(socket_rpc_server_test_autosave_state_bridge)
    IV_SUBSCRIBE_LINKER_EVENT(
        socket_rpc_server_test_autosave_state_bridge,
        iv_socket_rpc_get_autosave_state_event,
        &AutosaveStateFake::handle_get_autosave_state)
    IV_DECLARE_BRIDGE(
        socket_rpc_server_test_node_instances_bridge,
        iv::SocketRpcServer,
        NodeInstancesFake);
    IV_DEFINE_BRIDGE(socket_rpc_server_test_node_instances_bridge)
    IV_SUBSCRIBE_LINKER_EVENT(
        socket_rpc_server_test_node_instances_bridge,
        iv_socket_rpc_get_node_instances_event,
        &NodeInstancesFake::handle_get_node_instances)
    IV_DECLARE_BRIDGE(
        socket_rpc_server_test_package_definitions_bridge,
        iv::SocketRpcServer,
        PackageDefinitionsFake);
    IV_DEFINE_BRIDGE(socket_rpc_server_test_package_definitions_bridge)
    IV_SUBSCRIBE_LINKER_EVENT(
        socket_rpc_server_test_package_definitions_bridge,
        iv_socket_rpc_get_iv_package_definitions_event,
        &PackageDefinitionsFake::handle_get_package_definitions)
    IV_DECLARE_BRIDGE(
        socket_rpc_server_test_project_graph_bridge,
        iv::SocketRpcServer,
        ProjectGraphFake);
    IV_DEFINE_BRIDGE(socket_rpc_server_test_project_graph_bridge)
    IV_SUBSCRIBE_LINKER_EVENT(
        socket_rpc_server_test_project_graph_bridge,
        iv_socket_rpc_get_node_event_protocol_event,
        &ProjectGraphFake::handle_get_node_event_protocol)
    IV_SUBSCRIBE_LINKER_EVENT(
        socket_rpc_server_test_project_graph_bridge,
        iv_socket_rpc_delete_node_instance_event,
        &ProjectGraphFake::handle_delete_node_instance)
    IV_SUBSCRIBE_LINKER_EVENT(
        socket_rpc_server_test_project_graph_bridge,
        iv_socket_rpc_live_manual_control_update_event,
        &ProjectGraphFake::handle_live_manual_control_update)
    IV_SUBSCRIBE_LINKER_EVENT(
        socket_rpc_server_test_project_graph_bridge,
        iv_socket_rpc_begin_manual_control_interaction_event,
        &ProjectGraphFake::handle_begin_manual_control)
    IV_SUBSCRIBE_LINKER_EVENT(
        socket_rpc_server_test_project_graph_bridge,
        iv_socket_rpc_end_manual_control_interaction_event,
        &ProjectGraphFake::handle_end_manual_control)
    IV_SUBSCRIBE_LINKER_EVENT(
        socket_rpc_server_test_project_graph_bridge,
        iv_socket_rpc_client_disconnected_event,
        &ProjectGraphFake::handle_client_disconnected)
    IV_DECLARE_BRIDGE(socket_rpc_server_test_audio_graph_bridge, iv::SocketRpcServer, iv::ProjectGraph);
    IV_DEFINE_BRIDGE(socket_rpc_server_test_audio_graph_bridge)
    IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_server_test_audio_graph_bridge,
        iv_socket_rpc_set_audio_devices_event, &iv::ProjectGraph::handle_socket_rpc_set_audio_devices)
    IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_server_test_audio_graph_bridge,
        iv_socket_rpc_undo_project_edit_event, &iv::ProjectGraph::handle_socket_rpc_undo_project_edit)
    IV_SUBSCRIBE_LINKER_EVENT(socket_rpc_server_test_audio_graph_bridge,
        iv_socket_rpc_redo_project_edit_event, &iv::ProjectGraph::handle_socket_rpc_redo_project_edit)
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
        SessionProjectStateFake session_project_state;
        AutosaveStateFake autosave_state;
        NodeInstancesFake node_instances;
        PackageDefinitionsFake package_definitions;
        ProjectGraphFake project_graph;
        iv::ProjectGraph audio_project_graph;
        iv::project_graph_system_audio_devices_bridge::scope audio_owner_scope;
        socket_rpc_server_test_audio_graph_bridge::scope audio_history_scope;
        socket_rpc_server_test_session_project_state_bridge::scope session_project_state_scope;
        socket_rpc_server_test_autosave_state_bridge::scope autosave_state_scope;
        socket_rpc_server_test_node_instances_bridge::scope node_instances_scope;
        socket_rpc_server_test_package_definitions_bridge::scope package_definitions_scope;
        socket_rpc_server_test_project_graph_bridge::scope project_graph_scope;
        iv::socket_rpc_system_audio_devices_bridge::scope system_audio_devices_scope;
        std::string response_buffer;

        explicit SocketRpcHarness(std::filesystem::path const& workspace)
            : fds(make_socket_pair()),
              server(workspace, fds[1]),
              system_audio_devices(48000, 8, make_audio_backend()),
              test_state_scope(server, socket_rpc_test_state),
              audio_owner_scope(audio_project_graph, system_audio_devices),
              audio_history_scope(server, audio_project_graph),
              session_project_state_scope(server, session_project_state),
              autosave_state_scope(server, autosave_state),
              node_instances_scope(server, node_instances),
              package_definitions_scope(server, package_definitions),
              project_graph_scope(server, project_graph),
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

TEST(SocketRpcServer, SendsReadyNotificationAndStopsWhenItsOwnerDisconnects)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());

    auto const ready = harness.read_line();
    ASSERT_FALSE(ready.empty());
    EXPECT_TRUE(ready.contains(R"("method":"server.ready")")) << ready;

    // The inherited descriptor belongs to the process that launched the
    // server. Closing it stops the server without a shutdown request.
    harness.close_client_fd();
    auto stopped = std::async(std::launch::async, [&] { harness.server.wait(); });
    auto const status = stopped.wait_for(2s);
    if (status != std::future_status::ready) {
        harness.server.request_shutdown();
    }
    EXPECT_EQ(status, std::future_status::ready);
}

TEST(SocketRpcServer, DispatchesQueryEventAndReturnsSubscriberResult)
{
    socket_rpc_test_state.reset();
    socket_rpc_test_state.graph_query_result.nodes.push_back(iv::SymbolicNodeInfo {
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

TEST(SocketRpcServer, TransfersOneCapturedSnapshotWithLargeDurableCommandsAndRuntimeReads)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());
    ASSERT_FALSE(harness.read_line().empty());
    harness.node_instances.large_configurations.store(true);

    using Json = nlohmann::ordered_json;
    int id = 10;
    auto request = [&](std::string const& method, Json params = Json::object()) {
        auto const request_id = id++;
        harness.write_request(Json{{"jsonrpc", "2.0"}, {"id", request_id}, {"method", method},
            {"params", std::move(params)}}.dump() + "\n");
        auto const line = harness.read_response(request_id);
        EXPECT_LT(line.size(), 512 * 1024u);
        return Json::parse(line);
    };
    auto assemble = [&](Json reply) {
        if (!reply.contains("resultChunk")) return reply.at("result");
        auto const transfer_id = reply.at("resultChunk").at("transferId");
        std::string document;
        for (;;) {
            auto const& chunk = reply.at("resultChunk");
            EXPECT_EQ(chunk.at("transferId"), transfer_id);
            EXPECT_EQ(chunk.at("offset"), document.size());
            document += chunk.at("text").get<std::string>();
            auto const next = chunk.at("nextOffset");
            if (next.is_null()) break;
            EXPECT_EQ(next, document.size());
            reply = request("session.readResult", {{"transferId", transfer_id}, {"offset", next}});
        }
        EXPECT_TRUE(request("session.readResult", {{"transferId", transfer_id}, {"offset", document.size()}}).contains("error"));
        return Json::parse(document).at("result");
    };

    auto initial = request("session.snapshot");
    ASSERT_TRUE(initial.contains("resultChunk")) << initial.value("error", Json(nullptr));
    EXPECT_EQ(request("nodeInstances.delete", {{"instanceId", "other"}}).at("result").at("sessionRevision"), 1);
    auto const snapshot = assemble(std::move(initial));
    EXPECT_EQ(harness.session_project_state.captures.load(), 1u);
    EXPECT_EQ(snapshot.at("sessionRevision"), 0);
    EXPECT_EQ(snapshot.at("autosave").at("enabled"), true);
    EXPECT_EQ(snapshot.at("packages").at("packages").at(0).at("packageId"), "iv.test.snapshot");
    EXPECT_FALSE(snapshot.at("audioDevices").at("outputDevices").empty());
    EXPECT_EQ(snapshot.at("nodeEventProtocol").at("projectGraphGeneration"), 12);
    auto const& commands = snapshot.at("project").at("commands");
    ASSERT_EQ(commands.size(), 8u);
    EXPECT_TRUE(commands[0].at("args").at("configuration_source") == snapshot_configuration(540'000));
    auto const state = snapshot_lane_state();
    for (std::size_t index = 1; index < commands.size(); ++index) {
        EXPECT_EQ(commands[index].at("args").at("index"), index - 1);
        EXPECT_TRUE(commands[index].at("args").at("state") == state);
    }
    auto runtime = request("nodeInstances.get");
    ASSERT_TRUE(runtime.contains("resultChunk"));
    auto const instances = assemble(std::move(runtime));
    EXPECT_EQ(instances.at("sessionRevision"), 1);
    auto const& captured = snapshot.at("nodeInstances").at("instances");
    ASSERT_EQ(captured.size(), 3u);
    EXPECT_TRUE(instances.at("instances") == captured);
    EXPECT_TRUE(captured.at(2).at("configurationSource") == snapshot_configuration(360'000));
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

// A mutation's read-model changes return on its result builder and go out
// after the response, at the response's revision, without the handler
// notifying SocketRpcServer.
TEST(SocketRpcServer, PublishesAMutationsReadModelChangesAfterItsResponse)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());
    ASSERT_FALSE(harness.read_line().empty());

    harness.write_request(
        R"({"jsonrpc":"2.0","id":70,"method":"nodeInstances.delete","params":{"instanceId":"instance:gone"}})"
        "\n");
    auto const response = nlohmann::ordered_json::parse(harness.read_response(70));
    EXPECT_EQ(response["result"]["sessionRevision"], 1);

    auto const notification = nlohmann::ordered_json::parse(harness.read_line());
    EXPECT_EQ(notification["method"], "nodeInstances.updated");
    EXPECT_EQ(notification["params"]["revision"], 1);
    EXPECT_TRUE(notification["params"]["instances"].empty());
}

// A live gesture value takes no revision and no reply. Other clients get it
// as a replaceable frame; none follows the revision that commits its end.
TEST(SocketRpcServer, PublishesControlCompilationTimingsWithoutAnActivationClaim)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());
    ASSERT_FALSE(harness.read_line().empty());
    std::array statuses{
        iv::ProjectControlCompileStatus{.request_id = 7, .project_generation = 3,
            .phase = "queued", .dynamic = true, .predictive = true, .inputs = {"frequency"}},
        iv::ProjectControlCompileStatus{.request_id = 7, .project_generation = 3,
            .phase = "prepared", .dynamic = true, .predictive = true, .inputs = {"frequency"},
            .elapsed_us = 15000, .queue_us = 1000, .preparation_us = 15000},
    };
    harness.server.handle_control_compile_status(statuses);
    for (auto const& expected : statuses) {
        auto const json = nlohmann::ordered_json::parse(harness.read_line());
        EXPECT_EQ(json["method"], "graph.controlCompilation");
        EXPECT_EQ(json["params"]["requestId"], 7);
        EXPECT_EQ(json["params"]["phase"], expected.phase);
        EXPECT_EQ(json["params"]["preparationUs"], expected.preparation_us);
        EXPECT_FALSE(json["params"].contains("activationUs"));
    }
}

TEST(SocketRpcServer, InvalidControlPayloadKeepsItsRequestIdAndTheNextGestureCanBegin)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());
    ASSERT_FALSE(harness.read_line().empty());
    // A rejected hover must complete its own client request. Replying with
    // another ID leaves the client's entire ordered gesture queue waiting.
    harness.write_request(
        R"({"jsonrpc":"2.0","id":95,"method":"manualControls.hover","params":{"node":{"instanceId":"osc","path":[]},"inputName":"frequency","outputName":"main"}})" "\n");
    auto const rejected = nlohmann::ordered_json::parse(harness.read_response(95));
    EXPECT_EQ(rejected["id"], 95);
    ASSERT_TRUE(rejected.contains("error"));
    EXPECT_EQ(rejected["error"]["data"]["currentRevision"], 0);
    harness.write_request(
        R"({"jsonrpc":"2.0","id":96,"method":"manualControls.begin","params":{"interactionId":"drag","node":{"instanceId":"osc","path":[]},"inputName":"frequency","value":0.5}})" "\n");
    auto const started = nlohmann::ordered_json::parse(harness.read_response(96));
    ASSERT_TRUE(started.contains("result"));
    EXPECT_EQ(started["result"]["sessionRevision"], 1);
}

TEST(SocketRpcServer, RelaysLiveManualValuesToOtherClientsUntilTheGestureEnds)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());
    ASSERT_FALSE(harness.read_line().empty());
    auto second = make_socket_pair();
    harness.server.attach_client(second[1], true);
    std::string second_buffer;
    ASSERT_FALSE(read_line_until(second[0], &second_buffer, socket_rpc_startup_timeout).empty());

    harness.write_request(
        R"({"jsonrpc":"2.0","id":80,"method":"manualControls.begin","params":{"interactionId":"drag","node":{"instanceId":"osc","path":[]},"inputName":"frequency","value":0.25}})"
        "\n");
    EXPECT_EQ(nlohmann::ordered_json::parse(harness.read_response(80))["result"]["sessionRevision"], 1);
    EXPECT_EQ(nlohmann::ordered_json::parse(read_line_until(
        second[0], &second_buffer, socket_rpc_response_timeout))["method"], "session.revisionChanged");

    harness.write_request(
        R"({"jsonrpc":"2.0","method":"manualControls.update","params":{"interactionId":"drag","value":0.5}})"
        "\n");
    auto const frame = nlohmann::ordered_json::parse(read_line_until(
        second[0], &second_buffer, socket_rpc_response_timeout));
    EXPECT_EQ(frame["method"], "manualControls.liveValue");
    EXPECT_EQ(frame["params"]["inputName"], "frequency");
    EXPECT_EQ(frame["params"]["node"]["instanceId"], "osc");
    EXPECT_FLOAT_EQ(frame["params"]["value"].get<float>(), .5f);
    EXPECT_FALSE(frame["params"].contains("revision"));

    // The next value is still inside the relay interval when the end commits.
    harness.write_request(
        R"({"jsonrpc":"2.0","method":"manualControls.update","params":{"interactionId":"drag","value":0.75}})"
        "\n"
        R"({"jsonrpc":"2.0","id":81,"method":"manualControls.end","params":{"interactionId":"drag","value":0.75,"expectedRevision":1}})"
        "\n");
    EXPECT_EQ(nlohmann::ordered_json::parse(harness.read_response(81))["result"]["sessionRevision"], 2);
    for (;;) {
        auto const line = read_line_until(second[0], &second_buffer, socket_rpc_response_timeout);
        ASSERT_FALSE(line.empty());
        auto const message = nlohmann::ordered_json::parse(line);
        if (message["method"] == "session.revisionChanged") {
            EXPECT_EQ(message["params"]["revision"], 2);
            break;
        }
        EXPECT_EQ(message["method"], "manualControls.liveValue");
    }
    EXPECT_TRUE(read_line_until(second[0], &second_buffer, 150ms).empty());
    // The sender receives neither relays nor replies for its live values,
    // only the revisions of its begin and end.
    for (auto line = harness.read_line(150ms); !line.empty(); line = harness.read_line(150ms)) {
        auto const message = nlohmann::ordered_json::parse(line);
        EXPECT_FALSE(message.contains("id")) << line;
        EXPECT_EQ(message["method"], "session.revisionChanged") << line;
    }
    ::close(second[0]);
}

TEST(SocketRpcServer, PublishesOneRevisionWhenAClosedClientsGestureCommits)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());
    ASSERT_FALSE(harness.read_line().empty());
    auto gesturing = make_socket_pair();
    harness.server.attach_client(gesturing[1], true);
    std::string gesturing_buffer;
    ASSERT_FALSE(read_line_until(gesturing[0], &gesturing_buffer, socket_rpc_startup_timeout).empty());
    std::string const begin =
        R"({"jsonrpc":"2.0","id":90,"method":"manualControls.begin","params":{"interactionId":"drag","node":{"instanceId":"osc","path":[]},"inputName":"frequency","value":0.25}})"
        "\n";
    ASSERT_EQ(::write(gesturing[0], begin.data(), begin.size()), static_cast<ssize_t>(begin.size()));
    ASSERT_FALSE(read_response_for_id(gesturing[0], &gesturing_buffer, 90, socket_rpc_response_timeout).empty());
    EXPECT_EQ(nlohmann::ordered_json::parse(harness.read_line())["params"]["revision"], 1);

    ::close(gesturing[0]);
    auto const committed = nlohmann::ordered_json::parse(harness.read_line());
    EXPECT_EQ(committed["method"], "session.revisionChanged");
    EXPECT_EQ(committed["params"]["revision"], 2);
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
    // Losing the owner would stop the server, so the owner filters out the
    // oversized server.message and only the second client overflows.
    harness.write_request(
        R"({"jsonrpc":"2.0","id":64,"method":"session.subscribe","params":{"afterRevision":0,"topics":["server.status"]}})"
        "\n");
    ASSERT_FALSE(harness.read_response(64).empty());

    auto second = make_socket_pair();
    harness.server.attach_client(second[1], true);
    std::string second_buffer;
    ASSERT_FALSE(read_line_until(
        second[0], &second_buffer, socket_rpc_startup_timeout).empty());
    ASSERT_EQ(harness.server.client_count(), 2u);

    harness.server.send_server_message(iv::SocketRpcServerMessage{
        .level = "info",
        .message = std::string(2 * 1024 * 1024, 'x'),
    });

    auto const deadline = std::chrono::steady_clock::now() + 2s;
    while (harness.server.client_count() != 1
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_EQ(harness.server.client_count(), 1u);
    ::close(second[0]);

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
        EXPECT_EQ(descriptor["protocolVersion"], 0);
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

TEST(SocketRpcServer, AudioSelectionParticipatesInRevisionStampedProjectUndoRedo)
{
    socket_rpc_test_state.reset();
    auto harness = SocketRpcHarness(make_server_workspace());
    ASSERT_FALSE(harness.read_line().empty());
    harness.write_request(R"({"id":90,"method":"audioDevices.set","params":{"outputDeviceId":"out-1","inputDeviceId":null}})" "\n");
    auto selected = nlohmann::ordered_json::parse(harness.read_response(90));
    ASSERT_FALSE(selected.contains("error"));
    EXPECT_EQ(selected["result"]["selectedOutput"]["deviceId"], "out-1");
    harness.write_request(R"({"id":91,"method":"project.undo","params":{}})" "\n");
    auto undone = nlohmann::ordered_json::parse(harness.read_response(91));
    ASSERT_FALSE(undone.contains("error"));
    EXPECT_GT(undone["result"]["sessionRevision"].get<std::uint64_t>(), selected["result"]["sessionRevision"].get<std::uint64_t>());
    harness.write_request(R"({"id":92,"method":"audioDevices.get","params":{}})" "\n");
    auto state = nlohmann::ordered_json::parse(harness.read_response(92));
    EXPECT_EQ(state["result"]["selectedOutput"]["deviceId"], "default");
    harness.write_request(R"({"id":93,"method":"project.redo","params":{}})" "\n");
    auto redone = nlohmann::ordered_json::parse(harness.read_response(93));
    ASSERT_FALSE(redone.contains("error"));
    EXPECT_GT(redone["result"]["sessionRevision"].get<std::uint64_t>(), undone["result"]["sessionRevision"].get<std::uint64_t>());
    harness.write_request(R"({"id":94,"method":"audioDevices.get","params":{}})" "\n");
    state = nlohmann::ordered_json::parse(harness.read_response(94));
    EXPECT_EQ(state["result"]["selectedOutput"]["deviceId"], "out-1");
}
