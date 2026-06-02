#include "../module_test_utils.h"

#include <intravenous/linker_event.h>
#include <intravenous/runtime/iv_module_reload.h>
#include <intravenous/runtime/iv_module_reload_events.h>
#include <intravenous/runtime/startup_config.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>

namespace {
struct IvModuleReloadWitness {
    std::optional<iv::IvModuleReloadResults> results {};

    void reset()
    {
        results.reset();
    }
};

IvModuleReloadWitness *g_iv_module_reload_witness = nullptr;

iv::IvModuleDefinitionDeclaration make_declaration(std::filesystem::path module_root)
{
    auto const normalized = std::filesystem::weakly_canonical(module_root).lexically_normal();
    return iv::IvModuleDefinitionDeclaration{
        .definition_id = normalized.string(),
        .module_root = normalized,
    };
}

IV_SUBSCRIBE_LINKER_EVENT(
    iv::IvModuleReloadResultsEvent,
    iv_runtime_iv_module_reload_results_event,
    +[](iv::IvModuleReloadResults const &results) {
        if (g_iv_module_reload_witness != nullptr) {
            g_iv_module_reload_witness->results = results;
        }
    });

class IvModuleReloadTest : public ::testing::Test {
protected:
    IvModuleReloadWitness witness {};

    void SetUp() override
    {
        g_iv_module_reload_witness = &witness;
    }

    void TearDown() override
    {
        g_iv_module_reload_witness = nullptr;
    }
};
} // namespace

TEST_F(IvModuleReloadTest, CreatedDeclarationPublishesLoadedDefinition)
{
    auto const workspace =
        iv::test_support::read_only_module_fixture_workspace("local_cmake");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::IvModuleReload reload(startup);

    reload.handle_definition_declarations_changed(
        iv::IvModuleDefinitionDeclarationsChanged{
            .created = {make_declaration(workspace)},
        });

    ASSERT_TRUE(witness.results.has_value());
    ASSERT_EQ(witness.results->loaded.size(), 1u);
    EXPECT_TRUE(witness.results->failed.empty());
    EXPECT_EQ(witness.results->loaded.front().definition_id, std::filesystem::weakly_canonical(workspace).string());
    EXPECT_FALSE(witness.results->loaded.front().module_id.empty());
}

TEST_F(IvModuleReloadTest, InvalidDeclarationPublishesFailure)
{
    auto const workspace =
        iv::test_support::read_only_module_fixture_workspace("missing_export");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::IvModuleReload reload(startup);

    reload.handle_definition_declarations_changed(
        iv::IvModuleDefinitionDeclarationsChanged{
            .created = {make_declaration(workspace)},
        });

    ASSERT_TRUE(witness.results.has_value());
    EXPECT_TRUE(witness.results->loaded.empty());
    ASSERT_EQ(witness.results->failed.size(), 1u);
    EXPECT_EQ(witness.results->failed.front().definition_id, std::filesystem::weakly_canonical(workspace).string());
    EXPECT_FALSE(witness.results->failed.front().message.empty());
}

TEST_F(IvModuleReloadTest, ReloadChangedDefinitionsDoesNothingWithoutWatcherChanges)
{
    auto const workspace =
        iv::test_support::read_only_module_fixture_workspace("local_cmake");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::IvModuleReload reload(startup);

    reload.handle_definition_declarations_changed(
        iv::IvModuleDefinitionDeclarationsChanged{
            .created = {make_declaration(workspace)},
        });
    witness.reset();

    reload.reload_changed_definitions();

    EXPECT_FALSE(witness.results.has_value());
}
