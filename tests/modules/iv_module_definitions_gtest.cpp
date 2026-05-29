#include "../module_test_utils.h"

#include "runtime/iv_module_definitions.h"
#include "runtime/startup_config.h"

#include <gtest/gtest.h>

namespace {
using iv::test_support::fresh_module_fixture_workspace;
using iv::test_support::make_loaded_definition;
}

TEST(IvModuleDefinitions, SeedLoadedDefinitionPublishesLoadedSnapshot)
{
    auto const workspace =
        fresh_module_fixture_workspace("iv_module_definitions_seed_loaded_definition");

    iv::IvModuleDefinitions definitions;
    auto const loaded = make_loaded_definition(workspace);
    auto const definition_id = loaded.definition_id;

    definitions.seed_loaded_definition(loaded);

    auto const loaded_definitions = definitions.loaded_definitions();
    ASSERT_EQ(loaded_definitions.size(), 1u);
    EXPECT_EQ(loaded_definitions.front().definition_id, definition_id);
    EXPECT_EQ(loaded_definitions.front().module_root, std::filesystem::weakly_canonical(workspace));
    EXPECT_NE(loaded_definitions.front().canonical_builder, nullptr);
}

TEST(IvModuleDefinitions, RemoveDefinitionClearsLoadedSnapshot)
{
    auto const workspace =
        fresh_module_fixture_workspace("iv_module_definitions_remove_definition");

    iv::IvModuleDefinitions definitions;
    auto const loaded = make_loaded_definition(workspace);
    auto const definition_id = loaded.definition_id;
    definitions.seed_loaded_definition(loaded);

    definitions.remove_definition(definition_id);

    EXPECT_TRUE(definitions.loaded_definitions().empty());
}
