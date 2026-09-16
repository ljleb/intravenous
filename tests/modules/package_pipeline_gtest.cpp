#include "../module_test_utils.h"

#include <intravenous/bridge.h>
#include <intravenous/node/block_executor.h>
#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/package_definitions.h>
#include <intravenous/runtime/package_definitions_node_definitions_bridge.h>
#include <intravenous/runtime/package_jit.h>
#include <intravenous/runtime/package_watcher.h>
#include <intravenous/runtime/package_watcher_package_definitions_bridge.h>
#include <intravenous/runtime/package_watcher_package_jit_bridge.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/startup_config.h>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

namespace {
struct PackageWatcherStatusWitness {
    std::vector<iv::ProjectStatusNotification> statuses{};
    void handle_notification(iv::ProjectNotification const& notification)
    {
        if (auto const* status = std::get_if<iv::ProjectStatusNotification>(&notification)) {
            statuses.push_back(*status);
        }
    }
};

using namespace iv;
IV_DECLARE_BRIDGE(
    package_watcher_status_witness_bridge,
    iv::PackageWatcher,
    PackageWatcherStatusWitness);
IV_DEFINE_BRIDGE(package_watcher_status_witness_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    package_watcher_status_witness_bridge,
    iv_runtime_project_notification_event,
    &PackageWatcherStatusWitness::handle_notification)

std::pair<std::string, std::filesystem::path> package_declaration(
    std::string package_id,
    std::filesystem::path package_root)
{
    return {std::move(package_id), std::filesystem::weakly_canonical(package_root)};
}

std::pair<std::string, std::filesystem::path> default_package_declaration()
{
    auto root = std::filesystem::weakly_canonical(
        iv::test::repo_root() / "src/intravenous/builtin_packages/builtin");
    return {root.generic_string(), root};
}

struct PipelineHarness {
    iv::PackageWatcher watcher{};
    iv::PackageJit jit;
    iv::PackageDefinitions packages;
    iv::NodeDefinitions definitions{};
    PackageWatcherStatusWitness status_witness{};
    iv::package_watcher_package_jit_bridge::scope watcher_jit_scope;
    iv::package_watcher_package_definitions_bridge::scope watcher_packages_scope;
    iv::package_definitions_node_definitions_bridge::scope packages_definitions_scope;
    package_watcher_status_witness_bridge::scope status_scope;

    PipelineHarness(iv::StartupConfigState startup, std::filesystem::path project_root)
        : jit(std::move(startup), iv::ModuleLoader::OptimizationLevel::O0)
        , packages(std::move(project_root))
        , watcher_jit_scope(watcher, jit)
        , watcher_packages_scope(watcher, packages)
        , packages_definitions_scope(packages, definitions)
        , status_scope(watcher, status_witness)
    {}

    bool refresh()
    {
        iv::PackageWatcherRefreshRequest request;
        watcher.handle_refresh_requested(request);
        return request.changed;
    }
};
} // namespace

TEST(PackagePipeline, DirtyDeclarationsBuildAndPublishDefinitions)
{
    auto const workspace =
        iv::test_support::read_only_module_fixture_workspace("local_cmake");
    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    PipelineHarness pipeline(startup_config.initialize(), workspace);

    pipeline.watcher.synchronize_discovered_packages({
        default_package_declaration(),
        package_declaration("iv.test.local_cmake", workspace),
    });
    ASSERT_TRUE(pipeline.refresh());

    auto const package_snapshot = pipeline.packages.snapshot();
    ASSERT_EQ(package_snapshot->by_package_id.size(), 2u);
    EXPECT_TRUE(package_snapshot->by_package_id.contains("iv.test.local_cmake"));

    auto const modules = pipeline.definitions.loaded_module_definitions();
    ASSERT_EQ(modules.size(), 1u);
    EXPECT_EQ(modules.front().definition_id, "iv.test.local_cmake");
    EXPECT_TRUE(static_cast<bool>(modules.front().root));

    auto const leaves = pipeline.definitions.loaded_leaf_definitions();
    EXPECT_NE(
        std::ranges::find(leaves, "iv.builtin.constant", &iv::LeafNodeDefinition::definition_id),
        leaves.end());
}

TEST(PackagePipeline, FailedBuildKeepsAcceptedRevisionAndReportsFailure)
{
    auto const workspace = iv::test_support::fresh_module_fixture_workspace(
        "package_pipeline_failed_rebuild_keeps_revision");
    iv::test_support::copy_directory(
        iv::test_support::test_modules_root() / "local_cmake",
        workspace);
    iv::test_support::write_text(workspace / "iv_project.jsonl", "");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    PipelineHarness pipeline(startup_config.initialize(), workspace);
    pipeline.watcher.synchronize_discovered_packages({
        default_package_declaration(),
        package_declaration("iv.test.local_cmake", workspace),
    });
    ASSERT_TRUE(pipeline.refresh());

    auto const before_packages = pipeline.packages.snapshot();
    auto const accepted_before = before_packages->by_package_id.at("iv.test.local_cmake");
    auto const definitions_before = pipeline.definitions.snapshot();
    ASSERT_NE(accepted_before, nullptr);
    ASSERT_TRUE(definitions_before->by_id.contains("iv.test.local_cmake"));

    iv::test_support::write_text(
        workspace / "module.cpp",
        "#include <intravenous/dsl.h>\n"
        "this is deliberately invalid C++\n");
    iv::test::advance_write_time(workspace / "module.cpp");
    pipeline.watcher.poll_dependency_changes();
    ASSERT_TRUE(pipeline.watcher.has_pending_refresh());
    ASSERT_TRUE(pipeline.refresh());

    auto const after_packages = pipeline.packages.snapshot();
    ASSERT_TRUE(after_packages->by_package_id.contains("iv.test.local_cmake"));
    EXPECT_EQ(
        after_packages->by_package_id.at("iv.test.local_cmake"),
        accepted_before);
    EXPECT_EQ(pipeline.definitions.snapshot(), definitions_before);

    auto const packages = pipeline.packages.list_packages();
    auto const failed = std::ranges::find(
        packages,
        "iv.test.local_cmake",
        &iv::IvPackageInfo::package_id);
    ASSERT_NE(failed, packages.end());
    EXPECT_EQ(failed->build_state, iv::PackageBuildState::failed);
    EXPECT_FALSE(failed->build_message.empty());
}

TEST(PackagePipeline, SuccessfulBuildStatusIncludesElapsedTime)
{
    auto const workspace =
        iv::test_support::read_only_module_fixture_workspace("local_cmake");
    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    PipelineHarness pipeline(startup_config.initialize(), workspace);

    pipeline.watcher.synchronize_discovered_packages({
        default_package_declaration(),
        package_declaration("iv.test.local_cmake", workspace),
    });
    ASSERT_TRUE(pipeline.refresh());

    auto const completed = std::ranges::find_if(
        pipeline.status_witness.statuses,
        [](iv::ProjectStatusNotification const& status) {
            return status.code == "rebuildFinished";
        });
    ASSERT_NE(completed, pipeline.status_witness.statuses.end());
    EXPECT_TRUE(std::regex_match(
        completed->message,
        std::regex("IV package build ready to apply in [0-9]+ ms")));
}

TEST(PackagePipeline, CompiledModuleDefinitionPublishesUsableExecutionRoot)
{
    auto const workspace =
        iv::test_support::read_only_module_fixture_workspace("reload_sample_period");
    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    PipelineHarness pipeline(startup_config.initialize(), workspace);

    pipeline.watcher.synchronize_discovered_packages({
        package_declaration("iv.test.reload_sample_period", workspace),
    });
    ASSERT_TRUE(pipeline.refresh());

    auto const modules = pipeline.definitions.loaded_module_definitions();
    ASSERT_EQ(modules.size(), 1u);
    auto const root = modules.front().root;
    ASSERT_TRUE(static_cast<bool>(root));
    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(root),
        8,
        {},
        std::nullopt,
        iv::DEFAULT_EVENT_PORT_BUFFER_BASE_MULTIPLIER,
        48000);
    EXPECT_EQ(executor.sample_rate(), 48000u);
    EXPECT_NO_THROW(executor.tick_block(0));
}

TEST(PackagePipeline, RefreshDoesNothingWithoutSourceOrDependencyChanges)
{
    auto const workspace = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_without_changes");
    iv::test_support::copy_directory(
        iv::test_support::test_modules_root() / "local_cmake",
        workspace);
    iv::test_support::write_text(workspace / "iv_project.jsonl", "");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    PipelineHarness pipeline(startup_config.initialize(), workspace);
    pipeline.watcher.synchronize_discovered_packages({
        package_declaration("iv.test.local_cmake", workspace),
    });
    ASSERT_TRUE(pipeline.refresh());
    EXPECT_FALSE(pipeline.refresh());
}

TEST(PackagePipeline, FailedFirstBuildRemainsWatchedAndCanRecoverAfterSourceEdit)
{
    auto const workspace = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_failed_first_build_recovery");
    iv::test_support::write_text(workspace / "iv_project.jsonl", "");
    iv::test_support::write_text(
        workspace / "module.cpp",
        "// first attempt deliberately has no package manifest\n");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    PipelineHarness pipeline(startup_config.initialize(), workspace);
    pipeline.watcher.synchronize_discovered_packages({
        package_declaration("iv.test.recover", workspace),
    });
    ASSERT_TRUE(pipeline.refresh());
    ASSERT_EQ(pipeline.packages.list_packages().size(), 1u);
    EXPECT_EQ(
        pipeline.packages.list_packages().front().build_state,
        iv::PackageBuildState::failed);

    iv::test_support::write_text(
        workspace / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test_support::write_text(
        workspace / "module.cpp",
        "#include <intravenous/dsl.h>\n\n"
        "void recovered(iv::GraphBuilder& g) { g.outputs(); }\n"
        "IV_MODULE(\"iv.test.recovered\", recovered);\n");
    iv::test::advance_write_time(workspace / "module.cpp");

    pipeline.watcher.poll_dependency_changes();
    ASSERT_TRUE(pipeline.watcher.has_pending_refresh());
    ASSERT_TRUE(pipeline.refresh());

    auto const packages = pipeline.packages.list_packages();
    ASSERT_EQ(packages.size(), 1u);
    EXPECT_EQ(packages.front().build_state, iv::PackageBuildState::built);
    auto const definitions = pipeline.definitions.loaded_module_definitions();
    ASSERT_EQ(definitions.size(), 1u);
    EXPECT_EQ(definitions.front().definition_id, "iv.test.recovered");
}


TEST(PackagePipeline, NodeInstancesUsesJitOwnedTypedArgumentOperationsFromPublishedSnapshot)
{
    auto const workspace = iv::test_support::fresh_module_fixture_workspace(
        "node_instances_jit_typed_argument_operations");
    iv::test_support::write_text(workspace / "iv_project.jsonl", "");
    iv::test_support::write_text(
        workspace / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test_support::write_text(
        workspace / "module.cpp",
        R"cpp(#include <intravenous/dsl.h>
#include <stdexcept>

void node_instances_jit_config(iv::GraphBuilder& g, int value)
{
    if (value != 37) throw std::runtime_error("typed argument value was corrupted");
    g.outputs();
}
IV_MODULE("iv.test.node_instances_jit_config", node_instances_jit_config);
)cpp");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    PipelineHarness pipeline(startup_config.initialize(), workspace);
    pipeline.watcher.synchronize_discovered_packages({
        package_declaration("iv.test.node_instances_jit_package", workspace),
    });
    ASSERT_TRUE(pipeline.refresh());

    auto const snapshot = pipeline.definitions.snapshot();
    auto const definition = snapshot->by_id.find("iv.test.node_instances_jit_config");
    ASSERT_NE(definition, snapshot->by_id.end());
    auto const* module = std::get_if<iv::ModuleNodeDefinition>(&definition->second.definition);
    ASSERT_NE(module, nullptr);
    ASSERT_NE(module->provider.signature, nullptr);
    auto const* signature = module->provider.signature();
    ASSERT_NE(signature, nullptr);
    ASSERT_EQ(signature->argument_count, 1u);
    ASSERT_NE(signature->parameter_types, nullptr);
    ASSERT_NE(signature->parameter_operations, nullptr);
    ASSERT_NE(signature->parameter_operations[0], nullptr);
    ASSERT_NE(signature->parameter_operations[0]->copy_construct, nullptr);
    ASSERT_NE(signature->parameter_operations[0]->equal, nullptr);
    ASSERT_NE(signature->parameter_operations[0]->hash, nullptr);

    int first_value = 37;
    int second_value = 37;
    iv::details::ConfigurationArgument first_argument{
        .data = &first_value,
        .type = signature->parameter_types[0],
    };
    iv::details::ConfigurationArgument second_argument{
        .data = &second_value,
        .type = signature->parameter_types[0],
    };
    std::array first_arguments{first_argument};
    std::array second_arguments{second_argument};
    std::array requests{
        iv::NodeInstanceConfigurationRequest{
            .instance_id = "first",
            .definition_id = "iv.test.node_instances_jit_config",
            .arguments = first_arguments,
        },
        iv::NodeInstanceConfigurationRequest{
            .instance_id = "second",
            .definition_id = "iv.test.node_instances_jit_config",
            .arguments = second_arguments,
        },
    };

    iv::NodeInstances instances;
    iv::GraphBuilder root;
    auto result = instances.configure_and_embed(snapshot, root, requests);

    ASSERT_TRUE(result.diagnostics.empty());
    ASSERT_EQ(result.placements.size(), 2u);
    EXPECT_EQ(instances.configuration_cache_size(), 1u);
    EXPECT_EQ(
        result.placements.at("first").configured,
        result.placements.at("second").configured);
    EXPECT_NE(result.placements.at("first").root, result.placements.at("second").root);
}
