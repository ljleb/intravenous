#include "../module_test_utils.h"

#include <intravenous/bridge.h>
#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/startup_config.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using iv::test_support::fresh_module_fixture_workspace;
using iv::test_support::make_loaded_definition;

iv::IvPackageReloadedDefinition source_definition(
    std::filesystem::path const& package_root,
    std::string package_id,
    std::string module_id)
{
    auto definition = make_loaded_definition(package_root, std::move(module_id));
    definition.package_id = std::move(package_id);
    return definition;
}


iv::details::RegisteredSignature const* empty_configuration_signature()
{
    static constexpr iv::details::RegisteredSignature signature{};
    return &signature;
}

void test_module_provider(
    iv::GraphBuilder&,
    std::span<iv::details::ConfigurationArgument>)
{}

iv::NodeRef test_leaf_provider(
    iv::GraphBuilder&,
    std::span<iv::details::ConfigurationArgument>,
    iv::ChannelLayout const*)
{
    return {};
}

std::string canonical_package_id(std::filesystem::path const& package_root)
{
    return std::filesystem::weakly_canonical(package_root)
        .lexically_normal()
        .generic_string();
}

struct NodeDefinitionsWitness {
    std::vector<iv::IvPackageDeclarationsChanged> package_declaration_changes{};
    std::vector<iv::IvPackageDefinitionsChanged> package_definition_changes{};
    std::vector<std::shared_ptr<iv::NodeDefinitionsSnapshot const>> snapshots{};

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
    void handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged const& change)
    {
        snapshots.push_back(change.snapshot);
    }
};

using namespace iv;
IV_DECLARE_BRIDGE(
    node_definitions_witness_bridge,
    iv::NodeDefinitions,
    NodeDefinitionsWitness);
IV_DEFINE_BRIDGE(node_definitions_witness_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    node_definitions_witness_bridge,
    iv_runtime_iv_package_declarations_changed_event,
    &NodeDefinitionsWitness::handle_package_declarations_changed)
IV_SUBSCRIBE_LINKER_EVENT(
    node_definitions_witness_bridge,
    iv_runtime_iv_package_definitions_changed_event,
    &NodeDefinitionsWitness::handle_package_definitions_changed)
IV_SUBSCRIBE_LINKER_EVENT(
    node_definitions_witness_bridge,
    iv_runtime_node_definitions_snapshot_changed_event,
    &NodeDefinitionsWitness::handle_node_definitions_snapshot_changed)
}


TEST(NodeDefinitions, SnapshotIsImmutableVersionedAndUnifiesLeafAndModuleDefinitions)
{
    auto const workspace =
        fresh_module_fixture_workspace("node_definitions_versioned_snapshot");
    auto const package_id = canonical_package_id(workspace);

    iv::NodeDefinitions definitions;
    NodeDefinitionsWitness witness;
    node_definitions_witness_bridge::scope witness_scope{definitions, witness};

    auto initial = definitions.snapshot();
    ASSERT_NE(initial, nullptr);
    EXPECT_EQ(initial->generation, 0u);
    EXPECT_TRUE(initial->by_id.empty());

    definitions.declare_package(package_id, workspace);
    auto module = source_definition(workspace, package_id, "iv.test.module");
    module.provider = iv::NodeDefinitionProvider{
        .module_build = &test_module_provider,
        .signature = &empty_configuration_signature,
    };
    iv::IvPackageReloadResults first;
    first.packages.push_back({
        .package_id = package_id,
        .package_root = workspace,
    });
    first.loaded.push_back(module);
    first.node_types.push_back({
        .package_id = package_id,
        .node_type_id = "iv.test.leaf",
        .package_root = workspace,
        .provider = iv::NodeDefinitionProvider{
            .leaf_build = &test_leaf_provider,
            .signature = &empty_configuration_signature,
        },
    });
    definitions.handle_reload_results(first);

    auto snapshot_v1 = definitions.snapshot();
    ASSERT_NE(snapshot_v1, nullptr);
    EXPECT_EQ(snapshot_v1->generation, 1u);
    ASSERT_EQ(snapshot_v1->by_id.size(), 2u);
    auto const module_v1 = snapshot_v1->by_id.at("iv.test.module");
    auto const leaf_v1 = snapshot_v1->by_id.at("iv.test.leaf");
    EXPECT_EQ(module_v1.kind, iv::NodeDefinitionKind::module);
    EXPECT_EQ(leaf_v1.kind, iv::NodeDefinitionKind::leaf);
    EXPECT_GT(module_v1.version, 0u);
    EXPECT_GT(leaf_v1.version, 0u);
    EXPECT_NE(module_v1.version, leaf_v1.version);
    ASSERT_TRUE(std::holds_alternative<iv::ModuleNodeDefinition>(module_v1.definition));
    ASSERT_TRUE(std::holds_alternative<iv::LeafNodeDefinition>(leaf_v1.definition));
    auto const& module_provider = std::get<iv::ModuleNodeDefinition>(module_v1.definition);
    auto const& leaf_provider = std::get<iv::LeafNodeDefinition>(leaf_v1.definition);
    EXPECT_EQ(module_provider.provider.module_build, &test_module_provider);
    EXPECT_EQ(leaf_provider.provider.leaf_build, &test_leaf_provider);
    EXPECT_EQ(module_provider.provider.signature, &empty_configuration_signature);
    EXPECT_EQ(leaf_provider.provider.signature, &empty_configuration_signature);

    // Republishing this package is a new provider revision even when its stable
    // IDs are unchanged. The previous immutable snapshot remains unchanged.
    iv::IvPackageReloadResults second;
    second.packages.push_back({
        .package_id = package_id,
        .package_root = workspace,
    });
    auto module_v2_source = source_definition(workspace, package_id, "iv.test.module");
    module_v2_source.provider = module.provider;
    second.loaded.push_back(std::move(module_v2_source));
    second.node_types.push_back({
        .package_id = package_id,
        .node_type_id = "iv.test.leaf",
        .package_root = workspace,
        .provider = first.node_types.front().provider,
    });
    definitions.handle_reload_results(second);

    auto snapshot_v2 = definitions.snapshot();
    ASSERT_NE(snapshot_v2, nullptr);
    EXPECT_NE(snapshot_v1, snapshot_v2);
    EXPECT_EQ(snapshot_v1->generation, 1u);
    EXPECT_EQ(snapshot_v2->generation, 2u);
    EXPECT_EQ(snapshot_v1->by_id.at("iv.test.module").version, module_v1.version);
    EXPECT_EQ(snapshot_v1->by_id.at("iv.test.leaf").version, leaf_v1.version);
    EXPECT_GT(snapshot_v2->by_id.at("iv.test.module").version, module_v1.version);
    EXPECT_GT(snapshot_v2->by_id.at("iv.test.leaf").version, leaf_v1.version);

    ASSERT_EQ(witness.snapshots.size(), 2u);
    EXPECT_EQ(witness.snapshots[0], snapshot_v1);
    EXPECT_EQ(witness.snapshots[1], snapshot_v2);
}

TEST(NodeDefinitions, DefinitionCollisionDoesNotPublishPartialSnapshot)
{
    auto const first_root =
        fresh_module_fixture_workspace("node_definitions_snapshot_collision_a");
    auto const second_root =
        fresh_module_fixture_workspace("node_definitions_snapshot_collision_b");
    auto const first_id = canonical_package_id(first_root);
    auto const second_id = canonical_package_id(second_root);

    iv::NodeDefinitions definitions;
    NodeDefinitionsWitness witness;
    node_definitions_witness_bridge::scope witness_scope{definitions, witness};
    definitions.declare_package(first_id, first_root);
    definitions.declare_package(second_id, second_root);

    definitions.handle_reload_results(iv::IvPackageReloadResults{
        .packages = {{.package_id = first_id, .package_root = first_root}},
        .loaded = {source_definition(first_root, first_id, "iv.test.shared")},
    });
    auto before_collision = definitions.snapshot();
    ASSERT_EQ(before_collision->generation, 1u);
    ASSERT_EQ(before_collision->by_id.size(), 1u);

    definitions.handle_reload_results(iv::IvPackageReloadResults{
        .packages = {{.package_id = second_id, .package_root = second_root}},
        .node_types = {{
            .package_id = second_id,
            .node_type_id = "iv.test.shared",
            .package_root = second_root,
        }},
    });

    auto after_collision = definitions.snapshot();
    EXPECT_EQ(after_collision, before_collision);
    EXPECT_EQ(after_collision->generation, 1u);
    ASSERT_EQ(after_collision->by_id.size(), 1u);
    EXPECT_EQ(after_collision->by_id.at("iv.test.shared").kind,
              iv::NodeDefinitionKind::module);
    ASSERT_EQ(witness.snapshots.size(), 1u);
}

TEST(NodeDefinitions, SeedLoadedDefinitionPublishesLoadedSnapshot)
{
    auto const workspace =
        fresh_module_fixture_workspace("node_definitions_seed_loaded_definition");

    iv::NodeDefinitions definitions;
    auto const loaded = make_loaded_definition(workspace);
    auto const definition_id = loaded.definition_id;

    definitions.seed_loaded_definition(loaded);

    auto const loaded_definitions = definitions.loaded_module_definitions();
    ASSERT_EQ(loaded_definitions.size(), 1u);
    EXPECT_EQ(loaded_definitions.front().definition_id, definition_id);
    EXPECT_EQ(loaded_definitions.front().package_root, std::filesystem::weakly_canonical(workspace));
    EXPECT_EQ(static_cast<bool>(loaded_definitions.front().root), static_cast<bool>(loaded.root));
}

TEST(NodeDefinitions, RemovePackageClearsOwnedLoadedSnapshots)
{
    auto const workspace =
        fresh_module_fixture_workspace("node_definitions_remove_definition");

    iv::NodeDefinitions definitions;
    auto const loaded = make_loaded_definition(workspace);
    auto const package_id = loaded.package_id;
    definitions.seed_loaded_definition(loaded);
    auto before_remove = definitions.snapshot();
    ASSERT_EQ(before_remove->generation, 1u);
    ASSERT_EQ(before_remove->by_id.size(), 1u);

    definitions.remove_package(package_id);

    EXPECT_TRUE(definitions.loaded_module_definitions().empty());
    auto after_remove = definitions.snapshot();
    EXPECT_EQ(after_remove->generation, 2u);
    EXPECT_TRUE(after_remove->by_id.empty());
    EXPECT_NE(after_remove, before_remove);
}


TEST(NodeDefinitions, ReloadingOnePackagePreservesOtherProviderVersion)
{
    auto const first_root =
        fresh_module_fixture_workspace("node_definitions_versions_first");
    auto const second_root =
        fresh_module_fixture_workspace("node_definitions_versions_second");
    auto const first_id = canonical_package_id(first_root);
    auto const second_id = canonical_package_id(second_root);

    iv::NodeDefinitions definitions;
    definitions.declare_package(first_id, first_root);
    definitions.declare_package(second_id, second_root);
    definitions.handle_reload_results(iv::IvPackageReloadResults{
        .packages = {
            {.package_id = first_id, .package_root = first_root},
            {.package_id = second_id, .package_root = second_root},
        },
        .loaded = {
            source_definition(first_root, first_id, "iv.test.first"),
            source_definition(second_root, second_id, "iv.test.second"),
        },
    });

    auto first_snapshot = definitions.snapshot();
    ASSERT_EQ(first_snapshot->generation, 1u);
    auto const first_version = first_snapshot->by_id.at("iv.test.first").version;
    auto const second_version = first_snapshot->by_id.at("iv.test.second").version;

    definitions.handle_reload_results(iv::IvPackageReloadResults{
        .packages = {{.package_id = first_id, .package_root = first_root}},
        .loaded = {source_definition(first_root, first_id, "iv.test.first")},
    });

    auto second_snapshot = definitions.snapshot();
    EXPECT_EQ(second_snapshot->generation, 2u);
    EXPECT_GT(second_snapshot->by_id.at("iv.test.first").version, first_version);
    EXPECT_EQ(second_snapshot->by_id.at("iv.test.second").version, second_version);
}

TEST(NodeDefinitions, PackageReloadReplacesItsCompleteModuleSet)
{
    auto const source_root =
        fresh_module_fixture_workspace("node_definitions_source_replacement");
    auto const other_source_root =
        fresh_module_fixture_workspace("node_definitions_other_source");
    constexpr std::string_view package_id = "test.package";
    constexpr std::string_view other_package_id = "test.other_package";

    iv::NodeDefinitions definitions;
    definitions.declare_package(std::string(package_id), source_root);
    definitions.declare_package(std::string(other_package_id), other_source_root);

    iv::IvPackageReloadResults initial;
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

    auto loaded = definitions.loaded_module_definitions();
    ASSERT_EQ(loaded.size(), 3u);
    EXPECT_EQ(loaded[0].module_id, "iv.test.a");
    EXPECT_EQ(loaded[1].module_id, "iv.test.b");
    EXPECT_EQ(loaded[2].module_id, "iv.test.c");

    iv::IvPackageReloadResults replacement;
    replacement.packages.push_back({
        .package_id = std::string(package_id),
        .package_root = source_root,
    });
    replacement.loaded.push_back(
        source_definition(source_root, std::string(package_id), "iv.test.b"));
    definitions.handle_reload_results(replacement);

    loaded = definitions.loaded_module_definitions();
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded[0].module_id, "iv.test.b");
    EXPECT_EQ(loaded[1].module_id, "iv.test.c");
}

TEST(NodeDefinitions, RequiredDefinitionSourceFirstMovePublishesRemovalUntilReplacement)
{
    auto const source_root = fresh_module_fixture_workspace(
        "node_definitions_source_first_move_source");
    auto const destination_root = fresh_module_fixture_workspace(
        "node_definitions_source_first_move_destination");
    auto const source_package_id = canonical_package_id(source_root);
    auto const destination_package_id = canonical_package_id(destination_root);
    constexpr std::string_view definition_id = "iv.test.moved";

    iv::NodeDefinitions definitions;
    NodeDefinitionsWitness witness;
    node_definitions_witness_bridge::scope witness_scope{definitions, witness};
    definitions.declare_package(source_package_id, source_root);
    definitions.declare_package(destination_package_id, destination_root);
    definitions.handle_reload_results(iv::IvPackageReloadResults{
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
    definitions.handle_reload_results(iv::IvPackageReloadResults{
        .packages = {{
            .package_id = source_package_id,
            .package_root = source_root,
        }},
    });
    auto loaded = definitions.loaded_module_definitions();
    EXPECT_TRUE(loaded.empty());
    ASSERT_EQ(witness.package_definition_changes.size(), 1u);
    EXPECT_EQ(
        witness.package_definition_changes.front().module_definitions.deleted_definition_ids,
        std::vector<std::string>{std::string(definition_id)});

    // The destination can later recreate the ID from its own complete
    // candidate without any third save or retained stale provider.
    definitions.handle_reload_results(iv::IvPackageReloadResults{
        .packages = {{
            .package_id = destination_package_id,
            .package_root = destination_root,
        }},
        .loaded = {source_definition(
            destination_root, destination_package_id, std::string(definition_id))},
    });
    loaded = definitions.loaded_module_definitions();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().module_id, definition_id);
    EXPECT_EQ(loaded.front().package_id, destination_package_id);
}

TEST(NodeDefinitions, RequiredDefinitionDestinationFirstMoveHoldsCollisionUntilSourceDropsId)
{
    auto const source_root = fresh_module_fixture_workspace(
        "node_definitions_destination_first_move_source");
    auto const destination_root = fresh_module_fixture_workspace(
        "node_definitions_destination_first_move_destination");
    auto const source_package_id = canonical_package_id(source_root);
    auto const destination_package_id = canonical_package_id(destination_root);
    constexpr std::string_view definition_id = "iv.test.moved";

    iv::NodeDefinitions definitions;
    definitions.declare_package(source_package_id, source_root);
    definitions.declare_package(destination_package_id, destination_root);
    definitions.handle_reload_results(iv::IvPackageReloadResults{
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
    definitions.handle_reload_results(iv::IvPackageReloadResults{
        .packages = {{
            .package_id = destination_package_id,
            .package_root = destination_root,
        }},
        .loaded = {source_definition(
            destination_root, destination_package_id, std::string(definition_id))},
    });
    auto loaded = definitions.loaded_module_definitions();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().package_id, source_package_id);

    definitions.handle_reload_results(iv::IvPackageReloadResults{
        .packages = {{
            .package_id = source_package_id,
            .package_root = source_root,
        }},
    });
    loaded = definitions.loaded_module_definitions();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().package_id, destination_package_id);
}


TEST(NodeDefinitions, RequiredDefinitionsPropagateOneBatchedPackageDeclarationChange)
{
    auto const first_package =
        fresh_module_fixture_workspace("node_definitions_required_batch_a");
    auto const second_package =
        fresh_module_fixture_workspace("node_definitions_required_batch_b");

    iv::NodeDefinitions definitions;
    NodeDefinitionsWitness witness;
    node_definitions_witness_bridge::scope witness_scope{definitions, witness};

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

TEST(NodeDefinitions, DiscoveryReplacementKeepsRetainedInstancePackageDeclarations)
{
    auto const retained_root = fresh_module_fixture_workspace(
        "node_definitions_retained_package");
    auto const discovered_root = fresh_module_fixture_workspace(
        "node_definitions_discovered_package");
    auto const retained_id = std::filesystem::weakly_canonical(retained_root).generic_string();
    auto const discovered_id = std::filesystem::weakly_canonical(discovered_root).generic_string();

    iv::NodeDefinitions definitions;
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
