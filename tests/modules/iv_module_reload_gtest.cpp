#include "../module_test_utils.h"

#include <intravenous/linker_event.h>
#include <intravenous/node/block_executor.h>
#include <intravenous/runtime/iv_module_reload.h>
#include <intravenous/runtime/iv_module_reload_events.h>
#include <intravenous/runtime/startup_config.h>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace {
struct IvModuleReloadWitness {
    std::optional<iv::IvModuleReloadResults> results {};

    void reset()
    {
        results.reset();
    }
};

IvModuleReloadWitness *g_iv_module_reload_witness = nullptr;

struct SampleCapture {
    iv::Sample *observed = nullptr;

    auto inputs() const
    {
        return std::array<iv::InputConfig, 1>{{}};
    }

    auto outputs() const
    {
        return std::array<iv::OutputConfig, 0>{};
    }

    void tick_block(auto const &ctx) const
    {
        *observed = ctx.inputs[0].get();
    }
};

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

TEST_F(IvModuleReloadTest, RetainsSamplePeriodForCompiledDefinitionBuilders)
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

    auto builder = std::move(witness.results->loaded.front().canonical_builder);
    iv::GraphBuilder execution_builder;
    auto const embedded = execution_builder.embed_subgraph(builder);
    iv::Sample observed = 0.0f;
    execution_builder.node<SampleCapture>(&observed)(embedded[0]);
    execution_builder.outputs({});

    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(execution_builder.build_root_node().graph),
        8);
    executor.tick_block(0);

    EXPECT_NEAR(observed.value, 1.0f / 48000.0f, 1.0e-8f);
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
