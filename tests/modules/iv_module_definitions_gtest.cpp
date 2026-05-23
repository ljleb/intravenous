#include "../module_test_utils.h"

#include "runtime/iv_module_definitions.h"
#include "runtime/startup_config.h"

#include <gtest/gtest.h>

namespace {
using iv::test_support::copy_fixture_workspace;
}

TEST(RuntimeIvModuleDefinitions, SeedLoadedDefinitionPublishesLoadedSnapshot)
{
    auto const workspace =
        copy_fixture_workspace("iv_module_definitions_seed_loaded_definition", "local_cmake");
    iv::test_support::write_text(workspace / ".intravenous", "");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();

    iv::RuntimeIvModuleDefinitions definitions;
    auto const loaded = iv::test::load_runtime_iv_module_definition(
        startup,
        std::filesystem::weakly_canonical(workspace));
    auto const definition_id = loaded.definition_id;

    definitions.seed_loaded_definition(loaded);

    auto const loaded_definitions = definitions.loaded_definitions();
    ASSERT_EQ(loaded_definitions.size(), 1u);
    EXPECT_EQ(loaded_definitions.front().definition_id, definition_id);
    EXPECT_EQ(loaded_definitions.front().module_root, std::filesystem::weakly_canonical(workspace));
    EXPECT_NE(loaded_definitions.front().canonical_builder, nullptr);
}

TEST(RuntimeIvModuleDefinitions, RemoveDefinitionClearsLoadedSnapshot)
{
    auto const workspace =
        copy_fixture_workspace("iv_module_definitions_remove_definition", "local_cmake");
    iv::test_support::write_text(workspace / ".intravenous", "");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();

    iv::RuntimeIvModuleDefinitions definitions;
    auto const loaded = iv::test::load_runtime_iv_module_definition(
        startup,
        std::filesystem::weakly_canonical(workspace));
    auto const definition_id = loaded.definition_id;
    definitions.seed_loaded_definition(loaded);

    definitions.remove_definition(definition_id);

    EXPECT_TRUE(definitions.loaded_definitions().empty());
}
