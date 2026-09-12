#include "../module_test_utils.h"

#include <intravenous/bridge.h>
#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_definitions_events.h>
#include <intravenous/runtime/iv_module_instances.h>
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
    std::filesystem::path const& package_root,
    std::string package_id,
    std::string module_id)
{
    auto definition = make_loaded_definition(package_root, std::move(module_id));
    definition.package_id = std::move(package_id);
    return definition;
}

std::string canonical_package_id(std::filesystem::path const& package_root)
{
    return std::filesystem::weakly_canonical(package_root)
        .lexically_normal()
        .generic_string();
}

struct IvModuleDefinitionsWitness {
    std::vector<iv::IvPackageDeclarationsChanged> package_declaration_changes{};
    std::vector<iv::IvPackageDefinitionsChanged> package_definition_changes{};

    void handle_package_declarations_changed(
        iv::IvPackageDeclarationsChanged const& change)
    {
        package_declaration_changes.push_back(change);
    }
    void handle_package_definitions_changed(
        iv::IvPackageDefinitionsChanged const& change)
    {
        package_definition_changes.push_back(change);
    }
};

using namespace iv;
IV_DECLARE_BRIDGE(
    iv_module_definitions_witness_bridge,
    iv::IvModuleDefinitions,
    IvModuleDefinitionsWitness);
IV_DEFINE_BRIDGE(iv_module_definitions_witness_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    iv_module_definitions_witness_bridge,
    iv_runtime_iv_package_declarations_changed_event,
    &IvModuleDefinitionsWitness::handle_package_declarations_changed)
IV_SUBSCRIBE_LINKER_EVENT(
    iv_module_definitions_witness_bridge,
    iv_runtime_iv_package_definitions_changed_event,
    &IvModuleDefinitionsWitness::handle_package_definitions_changed)
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
    EXPECT_EQ(loaded_definitions.front().package_root, std::filesystem::weakly_canonical(workspace));
    EXPECT_EQ(static_cast<bool>(loaded_definitions.front().root), static_cast<bool>(loaded.root));
}

TEST(IvModuleDefinitions, RemovePackageClearsOwnedLoadedSnapshots)
{
    auto const workspace =
        fresh_module_fixture_workspace("iv_module_definitions_remove_definition");

    iv::IvModuleDefinitions definitions;
    auto const loaded = make_loaded_definition(workspace);
    auto const package_id = loaded.package_id;
    definitions.seed_loaded_definition(loaded);

    definitions.remove_package(package_id);

    EXPECT_TRUE(definitions.loaded_definitions().empty());
}

TEST(IvModuleDefinitions, PackageReloadReplacesItsCompleteModuleSet)
{
    auto const source_root =
        fresh_module_fixture_workspace("iv_module_definitions_source_replacement");
    auto const other_source_root =
        fresh_module_fixture_workspace("iv_module_definitions_other_source");
    constexpr std::string_view package_id = "test.package";
    constexpr std::string_view other_package_id = "test.other_package";

    iv::IvModuleDefinitions definitions;
    definitions.declare_package(std::string(package_id), source_root);
    definitions.declare_package(std::string(other_package_id), other_source_root);

    iv::IvModuleReloadResults initial;
    initial.packages.push_back({
        .package_id = std::string(package_id),
        .package_root = source_root,
    });
    initial.packages.push_back({
        .package_id = std::string(other_package_id),
        .package_root = other_source_root,
    });
    initial.loaded.push_back(source_definition(source_root, std::string(package_id), "iv.test.a"));
    initial.loaded.push_back(source_definition(source_root, std::string(package_id), "iv.test.b"));
    initial.loaded.push_back(source_definition(
        other_source_root, std::string(other_package_id), "iv.test.c"));
    definitions.handle_reload_results(initial);

    auto loaded = definitions.loaded_definitions();
    ASSERT_EQ(loaded.size(), 3u);
    EXPECT_EQ(loaded[0].module_id, "iv.test.a");
    EXPECT_EQ(loaded[1].module_id, "iv.test.b");
    EXPECT_EQ(loaded[2].module_id, "iv.test.c");

    iv::IvModuleReloadResults replacement;
    replacement.packages.push_back({
        .package_id = std::string(package_id),
        .package_root = source_root,
    });
    replacement.loaded.push_back(
        source_definition(source_root, std::string(package_id), "iv.test.b"));
    definitions.handle_reload_results(replacement);

    loaded = definitions.loaded_definitions();
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded[0].module_id, "iv.test.b");
    EXPECT_EQ(loaded[1].module_id, "iv.test.c");
}

TEST(IvModuleDefinitions, RequiredDefinitionSourceFirstMovePublishesRemovalUntilReplacement)
{
    auto const source_root = fresh_module_fixture_workspace(
        "iv_module_definitions_source_first_move_source");
    auto const destination_root = fresh_module_fixture_workspace(
        "iv_module_definitions_source_first_move_destination");
    auto const source_package_id = canonical_package_id(source_root);
    auto const destination_package_id = canonical_package_id(destination_root);
    constexpr std::string_view definition_id = "iv.test.moved";

    iv::IvModuleDefinitions definitions;
    IvModuleDefinitionsWitness witness;
    iv_module_definitions_witness_bridge::scope witness_scope{definitions, witness};
    definitions.declare_package(source_package_id, source_root);
    definitions.declare_package(destination_package_id, destination_root);
    definitions.handle_reload_results(iv::IvModuleReloadResults{
        .packages = {{
            .package_id = source_package_id,
            .package_root = source_root,
        }},
        .loaded = {source_definition(
            source_root, source_package_id, std::string(definition_id))},
    });
    definitions.handle_required_definitions_changed(
        iv::IvModuleRequiredDefinitionsChanged{
            .created = {{
                .definition_id = std::string(definition_id),
                .package_root = source_root,
            }},
        });
    witness.package_definition_changes.clear();

    // Desired project instances do not make a missing definition a registry
    // conflict. Publish the successful empty source candidate; the instance
    // becomes unrealized but remains available for the user to delete.
    definitions.handle_reload_results(iv::IvModuleReloadResults{
        .packages = {{
            .package_id = source_package_id,
            .package_root = source_root,
        }},
    });
    auto loaded = definitions.loaded_definitions();
    EXPECT_TRUE(loaded.empty());
    ASSERT_EQ(witness.package_definition_changes.size(), 1u);
    EXPECT_EQ(
        witness.package_definition_changes.front().modules.deleted_definition_ids,
        std::vector<std::string>{std::string(definition_id)});

    // The destination can later recreate the ID from its own complete
    // candidate without any third save or retained stale provider.
    definitions.handle_reload_results(iv::IvModuleReloadResults{
        .packages = {{
            .package_id = destination_package_id,
            .package_root = destination_root,
        }},
        .loaded = {source_definition(
            destination_root, destination_package_id, std::string(definition_id))},
    });
    loaded = definitions.loaded_definitions();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().module_id, definition_id);
    EXPECT_EQ(loaded.front().package_id, destination_package_id);
}

TEST(IvModuleDefinitions, RequiredDefinitionDestinationFirstMoveHoldsCollisionUntilSourceDropsId)
{
    auto const source_root = fresh_module_fixture_workspace(
        "iv_module_definitions_destination_first_move_source");
    auto const destination_root = fresh_module_fixture_workspace(
        "iv_module_definitions_destination_first_move_destination");
    auto const source_package_id = canonical_package_id(source_root);
    auto const destination_package_id = canonical_package_id(destination_root);
    constexpr std::string_view definition_id = "iv.test.moved";

    iv::IvModuleDefinitions definitions;
    definitions.declare_package(source_package_id, source_root);
    definitions.declare_package(destination_package_id, destination_root);
    definitions.handle_reload_results(iv::IvModuleReloadResults{
        .packages = {{
            .package_id = source_package_id,
            .package_root = source_root,
        }},
        .loaded = {source_definition(
            source_root, source_package_id, std::string(definition_id))},
    });
    definitions.handle_required_definitions_changed(
        iv::IvModuleRequiredDefinitionsChanged{
            .created = {{
                .definition_id = std::string(definition_id),
                .package_root = source_root,
            }},
        });

    // Saving the destination first creates a duplicate candidate ID. Preserve
    // the prior live provider until the source candidate is complete too.
    definitions.handle_reload_results(iv::IvModuleReloadResults{
        .packages = {{
            .package_id = destination_package_id,
            .package_root = destination_root,
        }},
        .loaded = {source_definition(
            destination_root, destination_package_id, std::string(definition_id))},
    });
    auto loaded = definitions.loaded_definitions();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().package_id, source_package_id);

    definitions.handle_reload_results(iv::IvModuleReloadResults{
        .packages = {{
            .package_id = source_package_id,
            .package_root = source_root,
        }},
    });
    loaded = definitions.loaded_definitions();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().package_id, destination_package_id);
}


TEST(IvModuleDefinitions, RequiredDefinitionsPropagateOneBatchedPackageDeclarationChange)
{
    auto const first_package =
        fresh_module_fixture_workspace("iv_module_definitions_required_batch_a");
    auto const second_package =
        fresh_module_fixture_workspace("iv_module_definitions_required_batch_b");

    iv::IvModuleDefinitions definitions;
    IvModuleDefinitionsWitness witness;
    iv_module_definitions_witness_bridge::scope witness_scope{definitions, witness};

    definitions.handle_required_definitions_changed(
        iv::IvModuleRequiredDefinitionsChanged{
            .created = {
                {.definition_id = "iv.test.a", .package_root = first_package},
                {.definition_id = "iv.test.b", .package_root = first_package},
            },
            .updated = {
                {.definition_id = "iv.test.c", .package_root = second_package},
            },
        });

    ASSERT_EQ(witness.package_declaration_changes.size(), 1u);
    auto const& change = witness.package_declaration_changes.front();
    EXPECT_EQ(change.created.size(), 2u);
    EXPECT_TRUE(change.updated.empty());
    EXPECT_TRUE(change.deleted_package_ids.empty());

    auto const first_root = std::filesystem::weakly_canonical(first_package);
    auto const second_root = std::filesystem::weakly_canonical(second_package);
    auto contains_root = [&](std::filesystem::path const& root) {
        return std::ranges::any_of(
            change.created,
            [&](iv::IvPackageDeclaration const& declaration) {
                return declaration.package_root == root;
            });
    };
    EXPECT_TRUE(contains_root(first_root));
    EXPECT_TRUE(contains_root(second_root));
}

TEST(IvModuleDefinitions, DiscoveryReplacementKeepsRetainedInstancePackageDeclarations)
{
    auto const retained_root = fresh_module_fixture_workspace(
        "iv_module_definitions_retained_package");
    auto const discovered_root = fresh_module_fixture_workspace(
        "iv_module_definitions_discovered_package");
    auto const retained_id = std::filesystem::weakly_canonical(retained_root).generic_string();
    auto const discovered_id = std::filesystem::weakly_canonical(discovered_root).generic_string();

    iv::IvModuleDefinitions definitions;
    definitions.handle_required_definitions_changed(
        iv::IvModuleRequiredDefinitionsChanged{
            .created = {{
                .definition_id = "iv.test.retained",
                .package_root = retained_root,
            }},
        });

    definitions.sync_package_declarations({{
        discovered_id,
        discovered_root,
    }});
    auto snapshots = definitions.package_definition_snapshots();
    ASSERT_EQ(snapshots.size(), 2u);
    auto has_package = [&](std::string const& package_id) {
        return std::ranges::any_of(
            snapshots,
            [&](iv::IvPackageDefinitionSnapshot const& snapshot) {
                return snapshot.declaration.package_id == package_id;
            });
    };
    EXPECT_TRUE(has_package(discovered_id));
    EXPECT_TRUE(has_package(retained_id));

    // Discovery owns only its own snapshot. Dropping the discovered path must
    // not make a persisted instance's retained package disappear.
    definitions.sync_package_declarations({});
    snapshots = definitions.package_definition_snapshots();
    ASSERT_EQ(snapshots.size(), 1u);
    EXPECT_EQ(snapshots.front().declaration.package_id, retained_id);
}
