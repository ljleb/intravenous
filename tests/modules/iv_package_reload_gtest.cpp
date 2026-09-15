#include "../module_test_utils.h"

#include <intravenous/bridge.h>
#include <intravenous/node/block_executor.h>
#include <intravenous/runtime/iv_package_reload.h>
#include <intravenous/runtime/iv_package_reload_events.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/startup_config.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace {
struct IvPackageReloadWitness {
    std::optional<iv::IvPackageReloadResults> results {};

    void reset()
    {
        results.reset();
    }
    void handle_results(iv::IvPackageReloadResults const &value)
    {
        results = value;
    }
};

struct IvPackageReloadStatusWitness {
    std::vector<iv::ProjectStatusNotification> statuses {};

    void handle_notification(iv::ProjectNotification const &notification)
    {
        if (auto const *status = std::get_if<iv::ProjectStatusNotification>(&notification)) {
            statuses.push_back(*status);
        }
    }
};

using namespace iv;
IV_DECLARE_BRIDGE(iv_package_reload_witness_bridge, iv::IvPackageReload, IvPackageReloadWitness);
IV_DECLARE_BRIDGE(
    iv_package_reload_status_witness_bridge,
    iv::IvPackageReload,
    IvPackageReloadStatusWitness);
IV_DEFINE_BRIDGE(iv_package_reload_witness_bridge)
IV_DEFINE_BRIDGE(iv_package_reload_status_witness_bridge)

iv::IvPackageDeclaration make_package_declaration(
    std::string_view package_id,
    std::filesystem::path package_root)
{
    auto const normalized = std::filesystem::weakly_canonical(package_root).lexically_normal();
    return iv::IvPackageDeclaration{
        .package_id = std::string(package_id),
        .package_root = normalized,
    };
}

iv::IvPackageDeclaration test_default_package_declaration()
{
    auto const root = iv::test::repo_root()
        / "src/intravenous/builtin_packages/builtin";
    return make_package_declaration(
        std::filesystem::weakly_canonical(root).generic_string(), root);
}

IV_SUBSCRIBE_LINKER_EVENT(
    iv_package_reload_witness_bridge,
    iv_runtime_iv_package_reload_results_event,
    &IvPackageReloadWitness::handle_results)
IV_SUBSCRIBE_LINKER_EVENT(
    iv_package_reload_status_witness_bridge,
    iv_runtime_project_notification_event,
    &IvPackageReloadStatusWitness::handle_notification)

class IvPackageReloadTest : public ::testing::Test {
protected:
    iv::IvPackageReload bridge_source {
        iv::StartupConfigState{},
        iv::ModuleLoader::OptimizationLevel::O0};
    IvPackageReloadWitness witness {};
    iv_package_reload_witness_bridge::scope witness_scope {bridge_source, witness};
    IvPackageReloadStatusWitness status_witness {};
    iv_package_reload_status_witness_bridge::scope status_witness_scope {
        bridge_source,
        status_witness};
};
} // namespace

TEST_F(IvPackageReloadTest, DirtyDeclarationCompilesAndPublishesLoadedDefinition)
{
    auto const workspace =
        iv::test_support::read_only_module_fixture_workspace("local_cmake");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::IvPackageReload reload(
        startup, iv::ModuleLoader::OptimizationLevel::O0);

    reload.handle_package_declarations_changed(
        iv::IvPackageDeclarationsChanged{
            .created = {
                test_default_package_declaration(),
                make_package_declaration("iv.test.local_cmake", workspace),
            },
        });

    EXPECT_TRUE(reload.has_dirty_packages());
    EXPECT_FALSE(witness.results.has_value());

    reload.compile_dirty_packages();
    EXPECT_TRUE(reload.has_pending_results());
    EXPECT_FALSE(witness.results.has_value());

    auto const build_statuses = reload.package_build_statuses();
    ASSERT_EQ(build_statuses.size(), 2u);
    auto const local_status = std::ranges::find(
        build_statuses, "iv.test.local_cmake", &iv::IvPackageBuildStatus::package_id);
    ASSERT_NE(local_status, build_statuses.end());
    EXPECT_EQ(local_status->state, iv::IvPackageBuildState::built);
    EXPECT_TRUE(local_status->message.empty());

    reload.apply_pending_results();

    ASSERT_TRUE(witness.results.has_value());
    ASSERT_EQ(witness.results->loaded.size(), 1u);
    EXPECT_TRUE(witness.results->failed.empty());
    EXPECT_EQ(witness.results->loaded.front().definition_id, "iv.test.local_cmake");
    EXPECT_FALSE(witness.results->loaded.front().module_id.empty());
    EXPECT_TRUE(static_cast<bool>(witness.results->loaded.front().root));
}

TEST_F(IvPackageReloadTest, DirtyInvalidDeclarationCompilesAndPublishesFailure)
{
    auto const workspace =
        iv::test_support::read_only_module_fixture_workspace("missing_export");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::IvPackageReload reload(
        startup, iv::ModuleLoader::OptimizationLevel::O0);

    reload.handle_package_declarations_changed(
        iv::IvPackageDeclarationsChanged{
            .created = {make_package_declaration("iv.test.missing_export", workspace)},
        });

    EXPECT_TRUE(reload.has_dirty_packages());

    reload.compile_dirty_packages();
    EXPECT_TRUE(reload.has_pending_results());

    auto const build_statuses = reload.package_build_statuses();
    ASSERT_EQ(build_statuses.size(), 1u);
    EXPECT_EQ(build_statuses.front().package_id, "iv.test.missing_export");
    EXPECT_EQ(build_statuses.front().state, iv::IvPackageBuildState::failed);
    EXPECT_FALSE(build_statuses.front().message.empty());

    reload.apply_pending_results();

    ASSERT_TRUE(witness.results.has_value());
    EXPECT_TRUE(witness.results->loaded.empty());
    ASSERT_EQ(witness.results->failed.size(), 1u);
    EXPECT_EQ(witness.results->failed.front().package_id, "iv.test.missing_export");
    EXPECT_FALSE(witness.results->failed.front().message.empty());
}

TEST_F(IvPackageReloadTest, SuccessfulBuildStatusIncludesElapsedTime)
{
    auto const workspace =
        iv::test_support::read_only_module_fixture_workspace("local_cmake");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::IvPackageReload reload(
        startup, iv::ModuleLoader::OptimizationLevel::O0);
    reload.handle_package_declarations_changed(
        iv::IvPackageDeclarationsChanged{
            .created = {
                test_default_package_declaration(),
                make_package_declaration("iv.test.local_cmake", workspace),
            },
        });

    reload.compile_dirty_packages();

    auto const completed = std::ranges::find_if(
        status_witness.statuses,
        [](iv::ProjectStatusNotification const &status) {
            return status.code == "rebuildFinished";
        });
    ASSERT_NE(completed, status_witness.statuses.end());
    EXPECT_TRUE(std::regex_match(
        completed->message,
        std::regex("IV package build ready to apply in [0-9]+ ms")));
}

TEST_F(IvPackageReloadTest, CompiledDefinitionPublishesUsableExecutionRoot)
{
    auto const workspace =
        iv::test_support::read_only_module_fixture_workspace("reload_sample_period");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::IvPackageReload reload(
        startup, iv::ModuleLoader::OptimizationLevel::O0);

    reload.handle_package_declarations_changed(
        iv::IvPackageDeclarationsChanged{
            .created = {make_package_declaration("iv.test.reload_sample_period", workspace)},
        });
    reload.compile_dirty_packages();
    reload.apply_pending_results();

    ASSERT_TRUE(witness.results.has_value());
    ASSERT_EQ(witness.results->loaded.size(), 1u);

    auto const root = witness.results->loaded.front().root;
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

TEST_F(IvPackageReloadTest, ReloadChangedDefinitionsDoesNothingWithoutWatcherChanges)
{
    auto const workspace =
        iv::test_support::fresh_module_fixture_workspace(
            "iv_package_reload_without_watcher_changes");
    iv::test_support::copy_directory(
        iv::test_support::test_modules_root() / "local_cmake",
        workspace);
    iv::test_support::write_text(workspace / "iv_project.jsonl", "");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::IvPackageReload reload(
        startup, iv::ModuleLoader::OptimizationLevel::O0);

    reload.handle_package_declarations_changed(
        iv::IvPackageDeclarationsChanged{
            .created = {make_package_declaration("iv.test.local_cmake", workspace)},
        });
    witness.reset();

    reload.reload_changed_packages();

    EXPECT_FALSE(witness.results.has_value());
}
