#include "../module_test_utils.h"

#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/iv_module_sources.h>
#include <intravenous/runtime/iv_module_instances_iv_module_sources_bridge.h>
#include <intravenous/runtime/iv_module_instances_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/project_persistence.h>
#include <intravenous/runtime/project_persistence_iv_module_instances_bridge.h>
#include <intravenous/runtime/socket_rpc_iv_module_sources_bridge.h>
#include <intravenous/runtime/socket_rpc_iv_module_instances_bridge.h>
#include <intravenous/runtime/socket_rpc_project_persistence_bridge.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string_view>

namespace {
using Json = nlohmann::ordered_json;

Json parse_json_line(std::string_view line)
{
    return Json::parse(line);
}
} // namespace

TEST(SocketRpcIvModuleInstancesBridge, UnboundCreateEventLeavesResponseUnbuilt)
{
    iv::SocketRpcCreateIvModuleInstanceResultBuilder builder;

    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_create_iv_module_instance_event,
        iv::CreateIvModuleInstanceRequest{
            .module_id = "iv.test.module_a",
        },
        builder);

    EXPECT_THROW(static_cast<void>(builder.build(1)), std::runtime_error);
}

TEST(IvModuleSources, NewProjectSourcesReceiveTheSameTemplateCompileDatabase)
{
    auto const project_root = std::filesystem::temp_directory_path()
        / "intravenous_iv_module_sources_compile_commands_test";
    std::filesystem::remove_all(project_root);

    iv::IvModuleSources sources(project_root, {});
    auto const first = sources.create_project_source("first");
    auto const second = sources.create_project_source("second");

    auto read = [](std::filesystem::path const& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    auto const first_database = read(first.module_root / "compile_commands.json");
    auto const second_database = read(second.module_root / "compile_commands.json");

    EXPECT_FALSE(first_database.empty());
    EXPECT_EQ(second_database, first_database);

    std::filesystem::remove_all(project_root);
}

TEST(SocketRpcIvModuleInstancesBridge, BoundEventsCreateAndDeleteInstances)
{
    iv::IvModuleInstances instances;
    iv::IvModuleSourceIntrospection introspection;
    iv::IvModuleSources sources("/tmp", {iv::test::test_modules_root() / "local_cmake"});
    iv::ProjectPersistence persistence("/tmp", {});
    iv::SocketRpcServer server("/tmp", -1);
    auto iv_module_instances_iv_module_sources_scope =
        iv::iv_module_instances_iv_module_sources_bridge::bind(instances, sources);
    auto project_persistence_iv_module_instances_scope =
        iv::project_persistence_iv_module_instances_bridge::bind(
            persistence,
            instances);
    auto iv_module_instances_iv_module_source_introspection_scope =
        iv::iv_module_instances_iv_module_source_introspection_bridge::bind(
            instances,
            introspection);
    auto socket_rpc_iv_module_instances_scope =
        iv::socket_rpc_iv_module_instances_bridge::bind(server, instances);
    auto socket_rpc_iv_module_sources_scope =
        iv::socket_rpc_iv_module_sources_bridge::bind(server, sources);
    auto socket_rpc_project_persistence_scope =
        iv::socket_rpc_project_persistence_bridge::bind(server, persistence);

    iv::SocketRpcCreateIvModuleInstanceResultBuilder create_builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_create_iv_module_instance_event,
        iv::CreateIvModuleInstanceRequest{
            .module_id = "iv.test.local_cmake",
        },
        create_builder);
    auto const create_response = parse_json_line(create_builder.build(2));
    auto const created_instance_id =
        create_response["result"]["instanceId"].get<std::string>();
    EXPECT_FALSE(created_instance_id.empty());

    iv::SocketRpcAckResponseBuilder delete_builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_delete_iv_module_instance_event,
        iv::DeleteIvModuleInstanceRequest{
            .instance_id = created_instance_id,
        },
        delete_builder);
    auto const delete_response = parse_json_line(delete_builder.build(3));
    EXPECT_EQ(delete_response["result"]["ok"], true);

}

TEST(SocketRpcIvModuleInstancesBridge, BoundSetDefaultSilenceTtlUpdatesInstance)
{
    iv::IvModuleInstances instances;
    iv::IvModuleSourceIntrospection introspection;
    iv::IvModuleSources sources("/tmp", {iv::test::test_modules_root() / "local_cmake"});
    iv::ProjectPersistence persistence("/tmp", {});
    iv::SocketRpcServer server("/tmp", -1);
    auto iv_module_instances_iv_module_sources_scope =
        iv::iv_module_instances_iv_module_sources_bridge::bind(instances, sources);
    auto project_persistence_iv_module_instances_scope =
        iv::project_persistence_iv_module_instances_bridge::bind(
            persistence,
            instances);
    auto iv_module_instances_iv_module_source_introspection_scope =
        iv::iv_module_instances_iv_module_source_introspection_bridge::bind(
            instances,
            introspection);
    auto socket_rpc_iv_module_instances_scope =
        iv::socket_rpc_iv_module_instances_bridge::bind(server, instances);
    auto socket_rpc_iv_module_sources_scope =
        iv::socket_rpc_iv_module_sources_bridge::bind(server, sources);
    auto socket_rpc_project_persistence_scope =
        iv::socket_rpc_project_persistence_bridge::bind(server, persistence);

    iv::SocketRpcCreateIvModuleInstanceResultBuilder create_builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_create_iv_module_instance_event,
        iv::CreateIvModuleInstanceRequest{
            .module_id = "iv.test.local_cmake",
        },
        create_builder);
    auto const create_response = parse_json_line(create_builder.build(2));
    auto const created_instance_id =
        create_response["result"]["instanceId"].get<std::string>();

    iv::SocketRpcAckResponseBuilder ttl_builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_update_iv_module_instances_event,
        iv::UpdateIvModuleInstancesRequest{
            .updates = {iv::UpdateIvModuleInstance{
                .instance_id = created_instance_id,
                .default_silence_ttl_samples = 1234,
            }},
        },
        ttl_builder);
    auto const ttl_response = parse_json_line(ttl_builder.build(4));
    EXPECT_EQ(ttl_response["result"]["ok"], true);

    auto const listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    ASSERT_TRUE(listed.front().default_silence_ttl_samples.has_value());
    EXPECT_EQ(*listed.front().default_silence_ttl_samples, 1234u);

}
