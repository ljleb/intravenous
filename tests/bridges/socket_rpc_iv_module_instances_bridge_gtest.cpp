#include "../module_test_utils.h"

#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/package_definitions_node_definitions_bridge.h>
#include <intravenous/runtime/package_definitions.h>
#include <intravenous/runtime/node_definitions_iv_module_instances_bridge.h>
#include <intravenous/runtime/iv_module_instances_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/socket_rpc_iv_module_instances_bridge.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
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

TEST(PackageDefinitions, NewProjectPackagesReceivePackageSpecificSharedPchCompileDatabase)
{
    auto const project_root = std::filesystem::temp_directory_path()
        / "intravenous_iv_package_definitions_compile_commands_test";
    std::filesystem::remove_all(project_root);

    iv::PackageDefinitions packages(project_root);
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

    auto expect_compile_database = [](std::filesystem::path const& package_root,
                                      std::string const& database_text) {
        ASSERT_FALSE(database_text.empty());
        auto const database = Json::parse(database_text);
        ASSERT_TRUE(database.is_array());
        ASSERT_EQ(database.size(), 1u);
        auto const& command = database.front();
        ASSERT_TRUE(command.contains("directory"));
        ASSERT_TRUE(command.contains("file"));
        ASSERT_TRUE(command.contains("command"));
        EXPECT_EQ(
            command.at("directory").get<std::string>(),
            package_root.generic_string());
        EXPECT_EQ(
            command.at("file").get<std::string>(),
            (package_root / "module.cpp").generic_string());

        auto const command_line = command.at("command").get<std::string>();
        EXPECT_NE(command_line.find("-std=c++23"), std::string::npos);
        EXPECT_NE(command_line.find("-include-pch"), std::string::npos);
        EXPECT_NE(command_line.find("iv_dsl.pch"), std::string::npos);
        EXPECT_NE(command_line.find(package_root.generic_string()), std::string::npos);
        // clangd must parse the same DSL/PCH command as package compilation,
        // but must not execute the metadata-emitting compiler plugin.
        EXPECT_EQ(command_line.find("-fplugin"), std::string::npos);
    };
    expect_compile_database(first.package_root, first_database);
    expect_compile_database(second.package_root, second_database);

    // File creation and catalog publication are separate responsibilities.
    // The package catalog only changes when the definition pipeline publishes
    // package declarations.
    packages.handle_package_refresh(iv::PackageRefreshTransaction{
        .declarations = iv::IvPackageDeclarationsChanged{
            .created = {
                {.package_id = first.package_id, .package_root = first.package_root},
                {.package_id = second.package_id, .package_root = second.package_root},
            },
        },
    });

    auto const listed = packages.list_packages();
    ASSERT_EQ(listed.size(), 2u);
    EXPECT_EQ(listed[0].package_id, first.package_id);
    EXPECT_EQ(listed[1].package_id, second.package_id);
    EXPECT_EQ(listed[0].build_state, iv::PackageBuildState::queued);
    EXPECT_EQ(listed[1].build_state, iv::PackageBuildState::queued);

    std::filesystem::remove_all(project_root);
}

TEST(PackageDefinitions, ListsPublishedDefinitionsFromTheRegistrySnapshot)
{
    auto const package_root = iv::test::test_modules_root() / "local_cmake";
    auto const normalized_root = std::filesystem::weakly_canonical(package_root);
    iv::NodeDefinitions definitions;
    iv::PackageDefinitions packages("/tmp");
    auto definitions_scope = iv::package_definitions_node_definitions_bridge::bind(
        packages, definitions);

    auto definition = iv::test_support::make_loaded_definition(
        package_root, "iv.test.local_cmake");
    definition.package_id = normalized_root.generic_string();
    packages.handle_package_refresh(iv::PackageRefreshTransaction{
        .declarations = iv::IvPackageDeclarationsChanged{
            .created = {{
                .package_id = normalized_root.generic_string(),
                .package_root = normalized_root,
            }},
        },
        .successful_revisions = {iv::PackageRevision{
            .package_id = normalized_root.generic_string(),
            .package_root = normalized_root,
            .revision = 1,
            .module_definitions = {std::move(definition)},
        }},
    });

    auto const listed = packages.list_packages();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed.front().package_id, normalized_root.generic_string());
    EXPECT_EQ(listed.front().module_ids,
        std::vector<std::string>{"iv.test.local_cmake"});
    EXPECT_TRUE(listed.front().publication_message.empty());
}

TEST(PackageDefinitions, RegistryConflictIsNotReportedAsAnEmptyPackage)
{
    auto const workspace = iv::test::fresh_module_fixture_workspace(
        "iv_package_definitions_registry_conflict");
    auto const first_root = workspace / "first";
    auto const second_root = workspace / "second";
    std::filesystem::create_directories(first_root);
    std::filesystem::create_directories(second_root);

    iv::NodeDefinitions definitions;
    iv::PackageDefinitions packages(workspace);
    auto definitions_scope = iv::package_definitions_node_definitions_bridge::bind(
        packages, definitions);
    packages.handle_package_refresh(iv::PackageRefreshTransaction{
        .declarations = iv::IvPackageDeclarationsChanged{
            .created = {
                {.package_id = "iv.test.first", .package_root = first_root},
            },
        },
        .successful_revisions = {iv::PackageRevision{
            .package_id = "iv.test.first",
            .package_root = first_root,
            .revision = 1,
            .module_definitions = {iv::PackageModuleDefinition{
                .package_id = "iv.test.first",
                .definition_id = "iv.test.shared",
                .package_root = first_root,
                .module_id = "iv.test.shared",
            }},
        }},
    });
    packages.handle_package_refresh(iv::PackageRefreshTransaction{
        .declarations = iv::IvPackageDeclarationsChanged{
            .created = {
                {.package_id = "iv.test.second", .package_root = second_root},
            },
        },
        .successful_revisions = {iv::PackageRevision{
            .package_id = "iv.test.second",
            .package_root = second_root,
            .revision = 1,
            .module_definitions = {iv::PackageModuleDefinition{
                .package_id = "iv.test.second",
                .definition_id = "iv.test.shared",
                .package_root = second_root,
                .module_id = "iv.test.shared",
            }},
        }},
    });

    auto const listed = packages.list_packages();
    auto const second = std::ranges::find_if(
        listed,
        [](iv::IvPackageInfo const& package) {
            return package.package_id == "iv.test.second";
        });
    ASSERT_NE(second, listed.end());
    EXPECT_TRUE(second->module_ids.empty());
    EXPECT_EQ(second->build_state, iv::PackageBuildState::built);
    EXPECT_NE(
        second->publication_message.find("provided by multiple IV packages"),
        std::string::npos);
}

TEST(SocketRpcIvModuleInstancesBridge, BoundEventsCreateAndDeleteInstances)
{
    iv::IvModuleInstances instances;
    iv::IvModuleSourceIntrospection introspection;
    auto const module_root = iv::test::test_modules_root() / "local_cmake";
    iv::NodeDefinitions definitions;
    iv::SocketRpcServer server("/tmp", -1);
    auto node_definitions_iv_module_instances_scope =
        iv::node_definitions_iv_module_instances_bridge::bind(
            definitions, instances);
    auto definition = iv::test_support::make_loaded_definition(
        module_root, "iv.test.local_cmake");
    definition.package_id = std::filesystem::weakly_canonical(module_root).generic_string();
    definitions.seed_loaded_definition(std::move(definition));
    auto iv_module_instances_iv_module_source_introspection_scope =
        iv::iv_module_instances_iv_module_source_introspection_bridge::bind(
            instances,
            introspection);
    auto socket_rpc_iv_module_instances_scope =
        iv::socket_rpc_iv_module_instances_bridge::bind(server, instances);

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

TEST(SocketRpcIvModuleInstancesBridge, BoundUpdateRenamesInstance)
{
    iv::IvModuleInstances instances;
    iv::IvModuleSourceIntrospection introspection;
    auto const module_root = iv::test::test_modules_root() / "local_cmake";
    iv::NodeDefinitions definitions;
    iv::SocketRpcServer server("/tmp", -1);
    auto node_definitions_iv_module_instances_scope =
        iv::node_definitions_iv_module_instances_bridge::bind(
            definitions, instances);
    auto definition = iv::test_support::make_loaded_definition(
        module_root, "iv.test.local_cmake");
    definition.package_id = std::filesystem::weakly_canonical(module_root).generic_string();
    definitions.seed_loaded_definition(std::move(definition));
    auto iv_module_instances_iv_module_source_introspection_scope =
        iv::iv_module_instances_iv_module_source_introspection_bridge::bind(
            instances,
            introspection);
    auto socket_rpc_iv_module_instances_scope =
        iv::socket_rpc_iv_module_instances_bridge::bind(server, instances);

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

    iv::SocketRpcAckResponseBuilder update_builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_update_iv_module_instances_event,
        iv::UpdateIvModuleInstancesRequest{
            .updates = {iv::UpdateIvModuleInstance{
                .instance_id = created_instance_id,
                .display_name = "Lead",
            }},
        },
        update_builder);
    auto const update_response = parse_json_line(update_builder.build(4));
    EXPECT_EQ(update_response["result"]["ok"], true);

    auto const listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed.front().display_name, "Lead");

}
