#include "module_test_utils.h"

#include "runtime/socket_rpc_server.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
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

    std::string read_line(int fd)
    {
        std::string line;
        std::array<char, 256> buffer {};
        for (;;) {
            ssize_t count = ::read(fd, buffer.data(), buffer.size());
            if (count <= 0) {
                break;
            }
            line.append(buffer.data(), static_cast<size_t>(count));
            auto const newline = line.find('\n');
            if (newline != std::string::npos) {
                line.resize(newline);
                break;
            }
        }
        return line;
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

    std::string const initialize_request =
        R"({"jsonrpc":"2.0","id":1,"method":"server.initialize","params":{"workspaceRoot":")" +
        std::filesystem::weakly_canonical(workspace).generic_string() +
        R"("}})" "\n";
    ASSERT_EQ(::write(fd, initialize_request.data(), initialize_request.size()), static_cast<ssize_t>(initialize_request.size()));

    auto const initialize_response = read_line(fd);
    EXPECT_TRUE(initialize_response.contains(R"("jsonrpc":"2.0")"));
    EXPECT_TRUE(initialize_response.contains(R"("executionEpoch":1)"));
    EXPECT_TRUE(initialize_response.contains(R"("moduleRoot":")"));

    std::string const query_request =
        R"({"jsonrpc":"2.0","id":2,"method":"graph.queryBySpans","params":{"filePath":")" +
        std::filesystem::weakly_canonical(workspace / "module.cpp").generic_string() +
        R"(","ranges":[{"start":{"line":7,"column":1},"end":{"line":15,"column":1}}]}})" "\n";
    ASSERT_EQ(::write(fd, query_request.data(), query_request.size()), static_cast<ssize_t>(query_request.size()));

    auto const query_response = read_line(fd);
    EXPECT_TRUE(query_response.contains(R"("jsonrpc":"2.0")"));
    EXPECT_TRUE(query_response.contains(R"("executionEpoch":1)"));
    EXPECT_TRUE(query_response.contains(R"("nodes":[)"));

    std::string const shutdown_request =
        R"({"jsonrpc":"2.0","id":3,"method":"server.shutdown","params":{}})" "\n";
    ASSERT_EQ(::write(fd, shutdown_request.data(), shutdown_request.size()), static_cast<ssize_t>(shutdown_request.size()));
    auto const shutdown_response = read_line(fd);
    EXPECT_TRUE(shutdown_response.contains(R"("ok":true)"));

    ::close(fd);
    server.request_shutdown();
}
