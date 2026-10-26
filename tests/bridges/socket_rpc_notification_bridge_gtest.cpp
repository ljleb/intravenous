#include "../module_test_utils.h"

#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/package_definitions_node_definitions_bridge.h>
#include <intravenous/runtime/package_definitions.h>
#include <intravenous/runtime/package_pipeline_events.h>
#include <intravenous/runtime/socket_rpc_package_definitions_bridge.h>
#include <intravenous/runtime/socket_rpc_node_instances_bridge.h>
#include <intravenous/runtime/project_persistence.h>
#include <intravenous/runtime/socket_rpc_project_persistence_bridge.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace iv {
namespace {
using namespace std::chrono_literals;
using Json = nlohmann::ordered_json;


constexpr auto socket_rpc_notification_startup_timeout = 30s;

std::string pop_line(std::string *buffer)
{
    auto const newline = buffer->find('\n');
    if (newline == std::string::npos) {
        return {};
    }
    auto line = buffer->substr(0, newline);
    buffer->erase(0, newline + 1);
    return line;
}

std::string read_line_until(int fd, std::string *buffer, std::chrono::milliseconds timeout)
{
    if (auto line = pop_line(buffer); !line.empty()) {
        return line;
    }

    auto const deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 256> read_buffer{};

    while (std::chrono::steady_clock::now() < deadline) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);

        auto const remaining =
            std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            break;
        }

        timeval tv{};
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
    std::array<int, 2> fds{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) != 0) {
        throw std::runtime_error(std::string("socketpair failed: ") + std::strerror(errno));
    }
    return fds;
}

class NotificationServerHarness {
    std::array<int, 2> fds;

public:
    SocketRpcServer server;
    std::string response_buffer;

    explicit NotificationServerHarness(std::filesystem::path const &workspace)
        : fds(make_socket_pair()), server(workspace, fds[1])
    {
        server.start();
        if (!server.wait_until_ready(socket_rpc_notification_startup_timeout)) {
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

    std::string read_line(std::chrono::milliseconds timeout = socket_rpc_notification_startup_timeout)
    {
        return read_line_until(client_fd(), &response_buffer, timeout);
    }
};

Json parse_json_line(std::string_view line)
{
    return Json::parse(line);
}
} // namespace
} // namespace iv

namespace iv {

TEST(SocketRpcNotificationBridge, UnboundServerDropsNotifications)
{
    ProjectNotification notification = ProjectMessageNotification{
        .level = "info",
        .message = "hello",
    };

    EXPECT_NO_THROW(IV_INVOKE_LINKER_EVENT(iv_runtime_project_notification_event, notification));
}

TEST(SocketRpcNotificationBridge, BoundServerForwardsNotificationVariants)
{
    auto harness = NotificationServerHarness(iv::test::fresh_module_fixture_workspace("socket_rpc_notification_server"));
    ProjectPersistence persistence("/tmp", {});
    auto notification_scope =
        socket_rpc_project_persistence_bridge::bind(
            harness.server,
            persistence);

    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_notification_event,
        ProjectNotification(ProjectMessageNotification{
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
        ProjectNotification(ProjectStatusNotification{
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


}



TEST(SocketRpcNotificationBridge, BoundServerForwardsIvModuleInstancesUpdated)
{
    auto harness = NotificationServerHarness(iv::test::fresh_module_fixture_workspace("socket_rpc_instances_notification_server"));
    NodeInstances instances;
    auto notification_scope =
        socket_rpc_node_instances_bridge::bind(
            harness.server,
            instances);

    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_iv_module_instances_list_changed_event,
        std::vector<iv::IvModuleInstanceInfo>{
            iv::IvModuleInstanceInfo{
                .instance_id = "instance:1",
                .definition_id = "/tmp/module-a",
                .package_root = "/tmp/module-a",
                .realized = true,
                .module_id = "module.a",
            },
            iv::IvModuleInstanceInfo{
                .instance_id = "instance:2",
                .definition_id = "/tmp/module-b",
                .package_root = "/tmp/module-b",
                .realized = false,
            },
        });

    auto const line = harness.read_line();
    ASSERT_FALSE(line.empty());
    auto const json = parse_json_line(line);
    EXPECT_EQ(json["method"], "ivModuleInstances.updated");
    ASSERT_EQ(json["params"]["instances"].size(), 2u);
    EXPECT_EQ(json["params"]["instances"][0]["instanceId"], "instance:1");
    EXPECT_EQ(json["params"]["instances"][0]["moduleId"], "module.a");
    EXPECT_EQ(json["params"]["instances"][1]["realized"], false);

}

TEST(SocketRpcNotificationBridge, PublishedPackageDefinitionsRefreshThePackageCatalog)
{
    auto const workspace = iv::test::fresh_module_fixture_workspace(
        "socket_rpc_package_catalog_notification_server");
    auto harness = NotificationServerHarness(workspace);
    NodeDefinitions definitions;
    PackageDefinitions package_definitions(workspace);
    auto definitions_scope = package_definitions_node_definitions_bridge::bind(
        package_definitions,
        definitions);
    auto package_catalog_scope = socket_rpc_package_definitions_bridge::bind(
        harness.server,
        package_definitions);

    package_definitions.handle_package_refresh(PackageRefreshTransaction{
        .declarations = IvPackageDeclarationsChanged{
            .created = {{.package_id = "iv.test.catalog", .package_root = workspace}},
        },
        .successful_revisions = {PackageRevision{
            .package_id = "iv.test.catalog",
            .package_root = workspace,
            .revision = 1,
            .module_definitions = {PackageModuleDefinition{
                .package_id = "iv.test.catalog",
                .definition_id = "iv.test.catalog.module",
                .package_root = workspace,
                .module_id = "iv.test.catalog.module",
            }},
        }},
    });
    IV_INVOKE_LINKER_EVENT_SOURCE(
        iv_runtime_iv_package_catalog_changed_event,
        IvPackageCatalogChanged{});

    auto const line = harness.read_line();
    ASSERT_FALSE(line.empty());
    auto const json = parse_json_line(line);
    EXPECT_EQ(json["method"], "ivPackages.updated");
    EXPECT_EQ(json["params"], Json::object());
}

} // namespace iv
