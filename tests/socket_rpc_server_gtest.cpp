#include "module_test_utils.h"

#include "runtime/socket_rpc_server.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
    using namespace std::chrono_literals;

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
            return std::span<iv::Sample>(_buffer.data(), _config.preferred_block_size * _config.num_channels);
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

    std::filesystem::path make_workspace(std::string const& name)
    {
        auto const workspace = iv::test::runtime_modules_root() / name;
        std::filesystem::remove_all(workspace);
        std::filesystem::create_directories(workspace);
        return workspace;
    }

    void write_text(std::filesystem::path const& path, std::string const& text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(static_cast<bool>(out)) << "failed to open " << path;
        out << text;
    }

    std::filesystem::path make_project_workspace()
    {
        auto const workspace = make_workspace("socket_rpc_server");
        iv::test::copy_directory(iv::test::test_modules_root() / "local_cmake", workspace);
        write_text(workspace / ".intravenous", "");
        return workspace;
    }

    std::filesystem::path make_polyphonic_project_workspace()
    {
        auto const workspace = make_workspace("socket_rpc_server_polyphonic");
        iv::test::copy_directory(iv::test::test_modules_root() / "local_cmake", workspace);
        write_text(workspace / ".intravenous", "");
        write_text(workspace / "module.cpp", R"(#include "dsl.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/shaping.h"

void polyphonic_module(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const& io = context.target_factory();
    auto const dt = g.node<ValueSource>(&context.sample_period());
    auto const sink = io.sink(g, 0);
    auto const voices = iv::polyphonic<2>(g, [&](auto m) {
        auto const saw = g.node<SawOscillator>();
        saw(
            "phase_offset"_P = 0.0,
            "frequency"_P = 440.0,
            "dt"_P = dt
        );
        return saw * ("amplitude"_P << m);
    });
    sink(voices);
    g.outputs();
}

IV_EXPORT_MODULE("iv.test.polyphonic_module", polyphonic_module);
)");
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

    std::string read_line(int fd, std::string* buffer)
    {
        if (auto line = pop_line(buffer); !line.empty()) {
            return line;
        }

        std::array<char, 256> read_buffer {};
        for (;;) {
            ssize_t count = ::read(fd, read_buffer.data(), read_buffer.size());
            if (count <= 0) {
                return {};
            }
            buffer->append(read_buffer.data(), static_cast<size_t>(count));
            if (auto line = pop_line(buffer); !line.empty()) {
                return line;
            }
        }
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
                deadline - std::chrono::steady_clock::now()
            );
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
        std::chrono::milliseconds timeout = 45s
    )
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
}

TEST(SocketRpcServer, InitializeAndQueryBySpansOverUnixSocket)
{
    auto const workspace = make_project_workspace();

    iv::SocketRpcServer server(workspace, iv::test::repo_root(), {}, make_audio_device_factory());
    server.start();
    ASSERT_TRUE(server.wait_until_ready(5s));

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0) << std::strerror(errno);

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    auto const socket_path = server.socket_path().string();
    ASSERT_LT(socket_path.size(), sizeof(address.sun_path));
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0) << std::strerror(errno);
    std::string response_buffer;

    std::string const initialize_request =
        R"({"jsonrpc":"2.0","id":1,"method":"server.initialize","params":{"workspaceRoot":")" +
        std::filesystem::weakly_canonical(workspace).generic_string() +
        R"("}})" "\n";
    ASSERT_EQ(::write(fd, initialize_request.data(), initialize_request.size()), static_cast<ssize_t>(initialize_request.size()));

    auto const initialize_response = read_line(fd, &response_buffer);
    EXPECT_TRUE(initialize_response.contains(R"("jsonrpc":"2.0")"));
    EXPECT_TRUE(initialize_response.contains(R"("executionEpoch":1)"));
    EXPECT_TRUE(initialize_response.contains(R"("moduleRoot":")"));
    EXPECT_TRUE(initialize_response.contains(R"("getLogicalNode":true)"));
    EXPECT_TRUE(initialize_response.contains(R"("getLogicalNodes":true)"));
    EXPECT_TRUE(initialize_response.contains(R"("queryActiveRegions":true)"));

    std::string const query_request =
        R"({"jsonrpc":"2.0","id":2,"method":"graph.queryBySpans","params":{"filePath":")" +
        std::filesystem::weakly_canonical(workspace / "module.cpp").generic_string() +
        R"(","ranges":[{"start":{"line":7,"column":1},"end":{"line":15,"column":1}}],"match":"intersection"}})" "\n";
    ASSERT_EQ(::write(fd, query_request.data(), query_request.size()), static_cast<ssize_t>(query_request.size()));

    auto const query_response = read_response_for_id(fd, &response_buffer, 2);
    ASSERT_FALSE(query_response.empty());
    EXPECT_TRUE(query_response.contains(R"("jsonrpc":"2.0")"));
    EXPECT_TRUE(query_response.contains(R"("executionEpoch":1)"));
    EXPECT_TRUE(query_response.contains(R"("nodes":[)"));
    EXPECT_TRUE(query_response.contains(R"("memberCount":)"));

    std::string const active_regions_request =
        R"({"jsonrpc":"2.0","id":6,"method":"graph.queryActiveRegions","params":{"filePath":")" +
        std::filesystem::weakly_canonical(workspace / "module.cpp").generic_string() +
        R"("}})" "\n";
    ASSERT_EQ(::write(fd, active_regions_request.data(), active_regions_request.size()), static_cast<ssize_t>(active_regions_request.size()));

    auto const active_regions_response = read_line(fd, &response_buffer);
    EXPECT_TRUE(active_regions_response.contains(R"("id":6)"));
    EXPECT_TRUE(active_regions_response.contains(R"("sourceSpans":[)"));

    std::smatch node_id_match;
    ASSERT_TRUE(std::regex_search(query_response, node_id_match, std::regex("\"id\":\"([^\"]+)\"")));
    auto const logical_node_id = node_id_match[1].str();

    std::string const get_logical_node_request =
        R"({"jsonrpc":"2.0","id":3,"method":"graph.getLogicalNode","params":{"executionEpoch":1,"nodeId":")" +
        logical_node_id +
        R"("}})" "\n";
    ASSERT_EQ(::write(fd, get_logical_node_request.data(), get_logical_node_request.size()), static_cast<ssize_t>(get_logical_node_request.size()));

    auto const get_logical_node_response = read_line(fd, &response_buffer);
    EXPECT_TRUE(get_logical_node_response.contains(R"("id":3)"));
    EXPECT_TRUE(get_logical_node_response.contains(R"("memberCount":)"));

    std::string const get_logical_nodes_request =
        R"({"jsonrpc":"2.0","id":4,"method":"graph.getLogicalNodes","params":{"executionEpoch":1,"nodeIds":[")" +
        logical_node_id +
        R"("]}})" "\n";
    ASSERT_EQ(::write(fd, get_logical_nodes_request.data(), get_logical_nodes_request.size()), static_cast<ssize_t>(get_logical_nodes_request.size()));

    auto const get_logical_nodes_response = read_line(fd, &response_buffer);
    EXPECT_TRUE(get_logical_nodes_response.contains(R"("id":4)"));
    EXPECT_TRUE(get_logical_nodes_response.contains(R"([{"id":")"));

    std::string const shutdown_request =
        R"({"jsonrpc":"2.0","id":5,"method":"server.shutdown","params":{}})" "\n";
    ASSERT_EQ(::write(fd, shutdown_request.data(), shutdown_request.size()), static_cast<ssize_t>(shutdown_request.size()));
    auto const shutdown_response = read_line(fd, &response_buffer);
    EXPECT_TRUE(shutdown_response.contains(R"("ok":true)"));

    ::close(fd);
    server.request_shutdown();
}

TEST(SocketRpcServer, SendsBuildNotificationsDuringReload)
{
    auto const workspace = make_project_workspace();

    iv::SocketRpcServer server(workspace, iv::test::repo_root(), {}, make_audio_device_factory());
    server.start();
    ASSERT_TRUE(server.wait_until_ready(5s));

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0) << std::strerror(errno);

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    auto const socket_path = server.socket_path().string();
    ASSERT_LT(socket_path.size(), sizeof(address.sun_path));
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0) << std::strerror(errno);
    std::string response_buffer;

    std::string const initialize_request =
        R"({"jsonrpc":"2.0","id":1,"method":"server.initialize","params":{"workspaceRoot":")" +
        std::filesystem::weakly_canonical(workspace).generic_string() +
        R"("}})" "\n";
    ASSERT_EQ(::write(fd, initialize_request.data(), initialize_request.size()), static_cast<ssize_t>(initialize_request.size()));
    auto const initialize_response = read_line(fd, &response_buffer);
    ASSERT_TRUE(initialize_response.contains(R"("executionEpoch":1)"));

    auto module_text = std::filesystem::exists(workspace / "module.cpp")
        ? std::ifstream(workspace / "module.cpp", std::ios::binary)
        : std::ifstream();
    ASSERT_TRUE(static_cast<bool>(module_text));
    std::string current {
        std::istreambuf_iterator<char>(module_text),
        std::istreambuf_iterator<char>()
    };
    write_text(workspace / "module.cpp", current + "\n");

    bool saw_started = false;
    bool saw_finished = false;
    bool saw_created_ids = false;
    bool saw_deleted_ids = false;
    auto const deadline = std::chrono::steady_clock::now() + 45s;
    while (std::chrono::steady_clock::now() < deadline && (!saw_started || !saw_finished)) {
        auto const line = read_line_until(fd, &response_buffer, 500ms);
        if (line.empty()) {
            continue;
        }
        if (line.contains(R"("method":"server.buildStarted")")) {
            saw_started = true;
        }
        if (line.contains(R"("method":"server.buildFinished")")) {
            saw_finished = true;
            saw_created_ids = line.contains(R"("createdNodeIds":[)");
            saw_deleted_ids = line.contains(R"("deletedNodeIds":[)");
        }
    }

    EXPECT_TRUE(saw_started);
    EXPECT_TRUE(saw_finished);
    EXPECT_TRUE(saw_created_ids);
    EXPECT_TRUE(saw_deleted_ids);

    std::string const shutdown_request =
        R"({"jsonrpc":"2.0","id":3,"method":"server.shutdown","params":{}})" "\n";
    ASSERT_EQ(::write(fd, shutdown_request.data(), shutdown_request.size()), static_cast<ssize_t>(shutdown_request.size()));
    auto const shutdown_response = read_line(fd, &response_buffer);
    EXPECT_TRUE(shutdown_response.contains(R"("ok":true)"));

    ::close(fd);
    server.request_shutdown();
}

TEST(SocketRpcServer, ReturnsPolyphonicCallbackLogicalMembersFromSocketApi)
{
    auto const workspace = make_polyphonic_project_workspace();

    iv::SocketRpcServer server(workspace, iv::test::repo_root(), {}, make_audio_device_factory());
    server.start();
    ASSERT_TRUE(server.wait_until_ready(5s));

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0) << std::strerror(errno);

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    auto const socket_path = server.socket_path().string();
    ASSERT_LT(socket_path.size(), sizeof(address.sun_path));
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0) << std::strerror(errno);
    std::string response_buffer;

    std::string const initialize_request =
        R"({"jsonrpc":"2.0","id":1,"method":"server.initialize","params":{"workspaceRoot":")" +
        std::filesystem::weakly_canonical(workspace).generic_string() +
        R"("}})" "\n";
    ASSERT_EQ(::write(fd, initialize_request.data(), initialize_request.size()), static_cast<ssize_t>(initialize_request.size()));
    auto const initialize_response = read_line(fd, &response_buffer);
    ASSERT_TRUE(initialize_response.contains(R"("executionEpoch":1)"));

    std::string const query_request =
        R"({"jsonrpc":"2.0","id":2,"method":"graph.queryBySpans","params":{"filePath":")" +
        std::filesystem::weakly_canonical(workspace / "module.cpp").generic_string() +
        R"(","ranges":[{"start":{"line":13,"column":20},"end":{"line":13,"column":20}}],"match":"intersection"}})" "\n";
    ASSERT_EQ(::write(fd, query_request.data(), query_request.size()), static_cast<ssize_t>(query_request.size()));
    auto const query_response = read_response_for_id(fd, &response_buffer, 2);
    ASSERT_FALSE(query_response.empty());
    EXPECT_TRUE(query_response.contains(R"("kind":"iv::SawOscillator")")) << query_response;
    EXPECT_TRUE(query_response.contains(R"("memberCount":2)")) << query_response;
    EXPECT_TRUE(query_response.contains(R"("memberNodes":[)")) << query_response;
    EXPECT_FALSE(query_response.contains(R"("kind":"Polyphonic")")) << query_response;
    EXPECT_FALSE(query_response.contains(R"("kind":"PolyphonicVoice")")) << query_response;

    std::smatch member_id_match;
    ASSERT_TRUE(std::regex_search(
        query_response,
        member_id_match,
        std::regex("\"memberNodes\":\\[\\{\"id\":\"([^\"]+)\",\"kind\":\"iv::SawOscillator\"")
    )) << query_response;
    auto const member_logical_node_id = member_id_match[1].str();

    std::string const get_members_request =
        R"({"jsonrpc":"2.0","id":3,"method":"graph.getLogicalNodes","params":{"executionEpoch":1,"nodeIds":[")" +
        member_logical_node_id +
        R"("]}})" "\n";
    ASSERT_EQ(::write(fd, get_members_request.data(), get_members_request.size()), static_cast<ssize_t>(get_members_request.size()));
    auto const members_response = read_response_for_id(fd, &response_buffer, 3);
    ASSERT_FALSE(members_response.empty());
    EXPECT_TRUE(members_response.contains(R"("kind":"iv::SawOscillator")"));
    EXPECT_TRUE(members_response.contains(R"("sampleInputs":[{"name":"phase_offset")"));
    EXPECT_TRUE(members_response.contains(R"("name":"frequency")"));
    EXPECT_TRUE(members_response.contains(R"("name":"dt")"));
    EXPECT_TRUE(members_response.contains(R"("sampleOutputs":[{"name":"out")"));
    EXPECT_TRUE(members_response.contains(R"("memberCount":1)"));

    std::string const shutdown_request =
        R"({"jsonrpc":"2.0","id":4,"method":"server.shutdown","params":{}})" "\n";
    ASSERT_EQ(::write(fd, shutdown_request.data(), shutdown_request.size()), static_cast<ssize_t>(shutdown_request.size()));
    auto const shutdown_response = read_line(fd, &response_buffer);
    EXPECT_TRUE(shutdown_response.contains(R"("ok":true)"));

    ::close(fd);
    server.request_shutdown();
}
