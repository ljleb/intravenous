#include "../module_test_utils.h"

#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/startup_config.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using iv::test_support::fresh_module_fixture_workspace;
using iv::test_support::make_loaded_definition;

iv::IvModuleReloadedDefinition source_definition(
    std::filesystem::path const& source_root,
    std::string source_id,
    std::string module_id)
{
    auto definition = make_loaded_definition(source_root, std::move(module_id));
    definition.source_id = std::move(source_id);
    return definition;
}
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
    EXPECT_EQ(static_cast<bool>(loaded_definitions.front().root), static_cast<bool>(loaded.root));
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

TEST(IvModuleDefinitions, SourceReloadReplacesItsCompleteModuleSet)
{
    auto const source_root =
        fresh_module_fixture_workspace("iv_module_definitions_source_replacement");
    auto const other_source_root =
        fresh_module_fixture_workspace("iv_module_definitions_other_source");
    constexpr std::string_view source_id = "test.source";
    constexpr std::string_view other_source_id = "test.other_source";

    iv::IvModuleDefinitions definitions;
    definitions.declare_definition(std::string(source_id), source_root);
    definitions.declare_definition(std::string(other_source_id), other_source_root);

    iv::IvModuleReloadResults initial;
    initial.sources.push_back({
        .definition_id = std::string(source_id),
        .module_root = source_root,
    });
    initial.sources.push_back({
        .definition_id = std::string(other_source_id),
        .module_root = other_source_root,
    });
    initial.loaded.push_back(source_definition(source_root, std::string(source_id), "iv.test.a"));
    initial.loaded.push_back(source_definition(source_root, std::string(source_id), "iv.test.b"));
    initial.loaded.push_back(source_definition(
        other_source_root, std::string(other_source_id), "iv.test.c"));
    definitions.handle_reload_results(initial);

    auto loaded = definitions.loaded_definitions();
    ASSERT_EQ(loaded.size(), 3u);
    EXPECT_EQ(loaded[0].module_id, "iv.test.a");
    EXPECT_EQ(loaded[1].module_id, "iv.test.b");
    EXPECT_EQ(loaded[2].module_id, "iv.test.c");

    iv::IvModuleReloadResults replacement;
    replacement.sources.push_back({
        .definition_id = std::string(source_id),
        .module_root = source_root,
    });
    replacement.loaded.push_back(
        source_definition(source_root, std::string(source_id), "iv.test.b"));
    definitions.handle_reload_results(replacement);

    loaded = definitions.loaded_definitions();
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded[0].module_id, "iv.test.b");
    EXPECT_EQ(loaded[1].module_id, "iv.test.c");
}
