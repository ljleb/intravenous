#include "../module_test_utils.h"

#include <intravenous/bridge.h>
#include <intravenous/node/block_executor.h>
#include <intravenous/runtime/iv_module_reload.h>
#include <intravenous/runtime/iv_module_reload_events.h>
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
struct IvModuleReloadWitness {
    std::optional<iv::IvModuleReloadResults> results {};

    void reset()
    {
        results.reset();
    }
    void handle_results(iv::IvModuleReloadResults const &value)
    {
        results = value;
    }
};

struct IvModuleReloadStatusWitness {
    std::vector<iv::ProjectStatusNotification> statuses {};

    void handle_notification(iv::ProjectNotification const &notification)
    {
        if (auto const *status = std::get_if<iv::ProjectStatusNotification>(&notification)) {
            statuses.push_back(*status);
        }
    }
};

using namespace iv;
IV_DECLARE_BRIDGE(iv_module_reload_witness_bridge, iv::IvModuleReload, IvModuleReloadWitness);
IV_DECLARE_BRIDGE(
    iv_module_reload_status_witness_bridge,
    iv::IvModuleReload,
    IvModuleReloadStatusWitness);
IV_DEFINE_BRIDGE(iv_module_reload_witness_bridge)
IV_DEFINE_BRIDGE(iv_module_reload_status_witness_bridge)

iv::IvModuleDefinitionDeclaration make_declaration(
    std::string_view definition_id,
    std::filesystem::path module_root)
{
    auto const normalized = std::filesystem::weakly_canonical(module_root).lexically_normal();
    return iv::IvModuleDefinitionDeclaration{
        .definition_id = std::string(definition_id),
        .module_root = normalized,
    };
}

IV_SUBSCRIBE_LINKER_EVENT(
    iv_module_reload_witness_bridge,
    iv_runtime_iv_module_reload_results_event,
    &IvModuleReloadWitness::handle_results)
IV_SUBSCRIBE_LINKER_EVENT(
    iv_module_reload_status_witness_bridge,
    iv_runtime_project_notification_event,
    &IvModuleReloadStatusWitness::handle_notification)

class IvModuleReloadTest : public ::testing::Test {
protected:
    iv::IvModuleReload bridge_source {iv::StartupConfigState{}};
    IvModuleReloadWitness witness {};
    iv_module_reload_witness_bridge::scope witness_scope {bridge_source, witness};
    IvModuleReloadStatusWitness status_witness {};
    iv_module_reload_status_witness_bridge::scope status_witness_scope {
        bridge_source,
        status_witness};
};
} // namespace

TEST_F(IvModuleReloadTest, DirtyDeclarationCompilesAndPublishesLoadedDefinition)
{
    auto const workspace =
        iv::test_support::read_only_module_fixture_workspace("local_cmake");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::IvModuleReload reload(startup);

    reload.handle_definition_declarations_changed(
        iv::IvModuleDefinitionDeclarationsChanged{
            .created = {make_declaration("iv.test.local_cmake", workspace)},
        });

    EXPECT_TRUE(reload.has_dirty_definitions());
    EXPECT_FALSE(witness.results.has_value());

    reload.compile_dirty_definitions();
    EXPECT_TRUE(reload.has_pending_results());
    EXPECT_FALSE(witness.results.has_value());

    reload.apply_pending_results();

    ASSERT_TRUE(witness.results.has_value());
    ASSERT_EQ(witness.results->loaded.size(), 1u);
    EXPECT_TRUE(witness.results->failed.empty());
    EXPECT_EQ(witness.results->loaded.front().definition_id, "iv.test.local_cmake");
    EXPECT_FALSE(witness.results->loaded.front().module_id.empty());
    EXPECT_TRUE(static_cast<bool>(witness.results->loaded.front().root));
}

TEST_F(IvModuleReloadTest, DirtyInvalidDeclarationCompilesAndPublishesFailure)
{
    auto const workspace =
        iv::test_support::read_only_module_fixture_workspace("missing_export");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::IvModuleReload reload(startup);

    reload.handle_definition_declarations_changed(
        iv::IvModuleDefinitionDeclarationsChanged{
            .created = {make_declaration("iv.test.missing_export", workspace)},
        });

    EXPECT_TRUE(reload.has_dirty_definitions());

    reload.compile_dirty_definitions();
    EXPECT_TRUE(reload.has_pending_results());

    reload.apply_pending_results();

    ASSERT_TRUE(witness.results.has_value());
    EXPECT_TRUE(witness.results->loaded.empty());
    ASSERT_EQ(witness.results->failed.size(), 1u);
    EXPECT_EQ(witness.results->failed.front().definition_id, "iv.test.missing_export");
    EXPECT_FALSE(witness.results->failed.front().message.empty());
}

TEST_F(IvModuleReloadTest, SuccessfulBuildStatusIncludesElapsedTime)
{
    auto const workspace =
        iv::test_support::read_only_module_fixture_workspace("local_cmake");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::IvModuleReload reload(startup);
    reload.handle_definition_declarations_changed(
        iv::IvModuleDefinitionDeclarationsChanged{
            .created = {make_declaration("iv.test.local_cmake", workspace)},
        });

    reload.compile_dirty_definitions();

    auto const completed = std::ranges::find_if(
        status_witness.statuses,
        [](iv::ProjectStatusNotification const &status) {
            return status.code == "rebuildFinished";
        });
    ASSERT_NE(completed, status_witness.statuses.end());
    EXPECT_TRUE(std::regex_match(
        completed->message,
        std::regex("Module build ready to apply in [0-9]+ ms")));
}

TEST_F(IvModuleReloadTest, CompiledDefinitionPublishesUsableExecutionRoot)
{
    auto const workspace =
        iv::test_support::read_only_module_fixture_workspace("reload_sample_period");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::IvModuleReload reload(startup);

    reload.handle_definition_declarations_changed(
        iv::IvModuleDefinitionDeclarationsChanged{
            .created = {make_declaration("iv.test.reload_sample_period", workspace)},
        });
    reload.compile_dirty_definitions();
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

TEST_F(IvModuleReloadTest, ReloadChangedDefinitionsDoesNothingWithoutWatcherChanges)
{
    auto const workspace =
        iv::test_support::fresh_module_fixture_workspace(
            "iv_module_reload_without_watcher_changes");
    iv::test_support::copy_directory(
        iv::test_support::test_modules_root() / "local_cmake",
        workspace);
    iv::test_support::write_text(workspace / "iv_project.jsonl", "");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::IvModuleReload reload(startup);

    reload.handle_definition_declarations_changed(
        iv::IvModuleDefinitionDeclarationsChanged{
            .created = {make_declaration("iv.test.local_cmake", workspace)},
        });
    witness.reset();

    reload.reload_changed_definitions();

    EXPECT_FALSE(witness.results.has_value());
}
