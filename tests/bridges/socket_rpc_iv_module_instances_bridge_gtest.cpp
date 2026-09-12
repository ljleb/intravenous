#include "../module_test_utils.h"

#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_reload.h>
#include <intravenous/runtime/iv_packages.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_instances_bridge.h>
#include <intravenous/runtime/iv_module_instances_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/project_persistence.h>
#include <intravenous/runtime/project_persistence_iv_module_instances_bridge.h>
#include <intravenous/runtime/socket_rpc_iv_packages_bridge.h>
#include <intravenous/runtime/socket_rpc_iv_module_instances_bridge.h>
#include <intravenous/runtime/socket_rpc_project_persistence_bridge.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

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

TEST(IvPackages, NewProjectPackagesReceiveTheSameTemplateCompileDatabase)
{
    auto const project_root = std::filesystem::temp_directory_path()
        / "intravenous_iv_packages_compile_commands_test";
    std::filesystem::remove_all(project_root);

    iv::IvModuleDefinitions definitions;
    iv::IvModuleReload reload({});
    iv::IvPackages packages(project_root, definitions, reload);
    auto const first = packages.create_project_package("first");
    auto const second = packages.create_project_package("second");

    EXPECT_TRUE(std::filesystem::exists(first.package_root / "iv_package.json"));
    EXPECT_TRUE(std::filesystem::exists(second.package_root / "iv_package.json"));

    auto read = [](std::filesystem::path const& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    auto const first_database = read(first.package_root / "compile_commands.json");
    auto const second_database = read(second.package_root / "compile_commands.json");

    EXPECT_FALSE(first_database.empty());
    EXPECT_EQ(second_database, first_database);

    auto const listed = packages.list_packages();
    ASSERT_EQ(listed.size(), 2u);
    EXPECT_EQ(listed[0].package_id, first.package_id);
    EXPECT_EQ(listed[1].package_id, second.package_id);
    EXPECT_EQ(listed[0].build_state, iv::IvPackageBuildState::queued);
    EXPECT_EQ(listed[1].build_state, iv::IvPackageBuildState::queued);

    std::filesystem::remove_all(project_root);
}

TEST(IvPackages, ListsPublishedDefinitionsFromTheRegistrySnapshot)
{
    auto const package_root = iv::test::test_modules_root() / "local_cmake";
    auto const normalized_root = std::filesystem::weakly_canonical(package_root);
    iv::IvModuleDefinitions definitions;
    iv::IvModuleReload reload({});
    iv::IvPackages packages("/tmp", definitions, reload);

    auto definition = iv::test_support::make_loaded_definition(
        package_root, "iv.test.local_cmake");
    definition.package_id = normalized_root.generic_string();
    definitions.seed_loaded_definition(std::move(definition));

    auto const listed = packages.list_packages();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed.front().package_id, normalized_root.generic_string());
    EXPECT_EQ(listed.front().module_ids,
        std::vector<std::string>{"iv.test.local_cmake"});
    EXPECT_TRUE(listed.front().publication_message.empty());
}

TEST(IvPackages, RegistryConflictIsNotReportedAsAnEmptyPackage)
{
    auto const workspace = iv::test::fresh_module_fixture_workspace(
        "iv_packages_registry_conflict");
    auto const first_root = workspace / "first";
    auto const second_root = workspace / "second";
    std::filesystem::create_directories(first_root);
    std::filesystem::create_directories(second_root);

    iv::IvModuleDefinitions definitions;
    iv::IvModuleReload reload({});
    iv::IvPackages packages(workspace, definitions, reload);
    definitions.seed_loaded_definition(iv::IvModuleReloadedDefinition{
        .package_id = "iv.test.first",
        .definition_id = "iv.test.shared",
        .package_root = first_root,
        .module_id = "iv.test.shared",
    });
    definitions.seed_loaded_definition(iv::IvModuleReloadedDefinition{
        .package_id = "iv.test.second",
        .definition_id = "iv.test.shared",
        .package_root = second_root,
        .module_id = "iv.test.shared",
    });

    auto const listed = packages.list_packages();
    auto const second = std::ranges::find_if(
        listed,
        [](iv::IvPackageInfo const& package) {
            return package.package_id == "iv.test.second";
        });
    ASSERT_NE(second, listed.end());
    EXPECT_TRUE(second->module_ids.empty());
    EXPECT_EQ(second->build_state, iv::IvPackageBuildState::queued);
    EXPECT_NE(
        second->publication_message.find("provided by multiple IV packages"),
        std::string::npos);
}

TEST(SocketRpcIvModuleInstancesBridge, BoundEventsCreateAndDeleteInstances)
{
    iv::IvModuleInstances instances;
    iv::IvModuleSourceIntrospection introspection;
    auto const module_root = iv::test::test_modules_root() / "local_cmake";
    iv::IvModuleDefinitions definitions;
    iv::IvModuleReload reload({});
    iv::IvPackages sources("/tmp", definitions, reload);
    iv::ProjectPersistence persistence("/tmp", {});
    iv::SocketRpcServer server("/tmp", -1);
    auto iv_module_definitions_iv_module_instances_scope =
        iv::iv_module_definitions_iv_module_instances_bridge::bind(
            definitions, instances);
    auto definition = iv::test_support::make_loaded_definition(
        module_root, "iv.test.local_cmake");
    definition.package_id = std::filesystem::weakly_canonical(module_root).generic_string();
    definitions.seed_loaded_definition(std::move(definition));
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
    auto socket_rpc_iv_packages_scope =
        iv::socket_rpc_iv_packages_bridge::bind(server, sources);
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
    auto const module_root = iv::test::test_modules_root() / "local_cmake";
    iv::IvModuleDefinitions definitions;
    iv::IvModuleReload reload({});
    iv::IvPackages sources("/tmp", definitions, reload);
    iv::ProjectPersistence persistence("/tmp", {});
    iv::SocketRpcServer server("/tmp", -1);
    auto iv_module_definitions_iv_module_instances_scope =
        iv::iv_module_definitions_iv_module_instances_bridge::bind(
            definitions, instances);
    auto definition = iv::test_support::make_loaded_definition(
        module_root, "iv.test.local_cmake");
    definition.package_id = std::filesystem::weakly_canonical(module_root).generic_string();
    definitions.seed_loaded_definition(std::move(definition));
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
    auto socket_rpc_iv_packages_scope =
        iv::socket_rpc_iv_packages_bridge::bind(server, sources);
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
