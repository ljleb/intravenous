#include "../module_test_utils.h"

#include <intravenous/bridge.h>
#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/node_definitions_events.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
using iv::test_support::fresh_module_fixture_workspace;
using iv::test_support::make_loaded_definition;

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

iv::PackageModuleDefinition module_definition(
    std::filesystem::path const& package_root,
    std::string package_id,
    std::string definition_id)
{
    auto definition = make_loaded_definition(package_root, definition_id);
    definition.package_id = std::move(package_id);
    definition.definition_id = definition_id;
    definition.module_id = std::move(definition_id);
    definition.provider = iv::NodeDefinitionProvider{
        .module_build = &test_module_provider,
        .signature = &empty_configuration_signature,
    };
    return definition;
}

iv::PackageLeafDefinition leaf_definition(
    std::filesystem::path const& package_root,
    std::string package_id,
    std::string definition_id)
{
    return iv::PackageLeafDefinition{
        .package_id = std::move(package_id),
        .definition_id = std::move(definition_id),
        .package_root = package_root,
        .provider = iv::NodeDefinitionProvider{
            .leaf_build = &test_leaf_provider,
            .signature = &empty_configuration_signature,
        },
    };
}

iv::PackageDefinitionsPublicationRequest publish(
    iv::NodeDefinitions& definitions,
    std::vector<iv::PackageRevision> revisions,
    std::uint64_t generation = 1)
{
    auto snapshot = std::make_shared<iv::PackageDefinitionsSnapshot>();
    snapshot->generation = generation;
    for (auto& revision : revisions) {
        auto package_id = revision.package_id;
        snapshot->by_package_id.emplace(
            package_id,
            std::make_shared<iv::PackageRevision const>(std::move(revision)));
    }
    iv::PackageDefinitionsPublicationRequest request{.snapshot = std::move(snapshot)};
    definitions.handle_package_definitions_publication(request);
    return request;
}

struct NodeDefinitionsWitness {
    std::vector<iv::IvPackageDefinitionsChanged> package_definition_changes{};
    std::vector<std::shared_ptr<iv::NodeDefinitionsSnapshot const>> snapshots{};

    void handle_package_definitions_changed(iv::IvPackageDefinitionsChanged const& change)
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
    iv_runtime_iv_package_definitions_changed_event,
    &NodeDefinitionsWitness::handle_package_definitions_changed)
IV_SUBSCRIBE_LINKER_EVENT(
    node_definitions_witness_bridge,
    iv_runtime_node_definitions_snapshot_changed_event,
    &NodeDefinitionsWitness::handle_node_definitions_snapshot_changed)
} // namespace

TEST(NodeDefinitions, SnapshotIsImmutableVersionedAndUnifiesLeafAndModuleDefinitions)
{
    auto const workspace = fresh_module_fixture_workspace(
        "node_definitions_versioned_snapshot");
    auto const package_id = canonical_package_id(workspace);

    iv::NodeDefinitions definitions;
    NodeDefinitionsWitness witness;
    node_definitions_witness_bridge::scope witness_scope{definitions, witness};

    auto initial = definitions.snapshot();
    ASSERT_NE(initial, nullptr);
    EXPECT_EQ(initial->generation, 0u);
    EXPECT_TRUE(initial->by_id.empty());

    auto module = module_definition(workspace, package_id, "iv.test.module");
    auto leaf = leaf_definition(workspace, package_id, "iv.test.leaf");
    auto first_result = publish(definitions, {iv::PackageRevision{
        .package_id = package_id,
        .package_root = workspace,
        .revision = 1,
        .module_definitions = {module},
        .leaf_definitions = {leaf},
    }});

    auto first = definitions.snapshot();
    ASSERT_EQ(first->by_id.size(), 2u);
    EXPECT_EQ(first->generation, 1u);
    auto const module_version = first->by_id.at("iv.test.module").version;
    auto const leaf_version = first->by_id.at("iv.test.leaf").version;
    EXPECT_EQ(
        first_result.published_module_ids_by_package_id.at(package_id),
        std::vector<std::string>{"iv.test.module"});
    EXPECT_EQ(
        first_result.published_leaf_ids_by_package_id.at(package_id),
        std::vector<std::string>{"iv.test.leaf"});

    auto second_result = publish(definitions, {iv::PackageRevision{
        .package_id = package_id,
        .package_root = workspace,
        .revision = 2,
        .module_definitions = {module},
        .leaf_definitions = {leaf},
    }}, 2);
    auto second = definitions.snapshot();
    EXPECT_EQ(second->generation, 2u);
    EXPECT_GT(second->by_id.at("iv.test.module").version, module_version);
    EXPECT_GT(second->by_id.at("iv.test.leaf").version, leaf_version);
    EXPECT_EQ(first->generation, 1u);
    EXPECT_EQ(first->by_id.at("iv.test.module").version, module_version);
    EXPECT_TRUE(second_result.publication_messages_by_package_id.empty());
    EXPECT_EQ(witness.snapshots.size(), 2u);
}

TEST(NodeDefinitions, CrossPackageCollisionKeepsPreviousPublishedNamespaceAndReturnsDiagnostics)
{
    auto const first_root = fresh_module_fixture_workspace("node_definitions_collision_a");
    auto const second_root = fresh_module_fixture_workspace("node_definitions_collision_b");
    auto const first_id = canonical_package_id(first_root);
    auto const second_id = canonical_package_id(second_root);

    iv::NodeDefinitions definitions;
    publish(definitions, {iv::PackageRevision{
        .package_id = first_id,
        .package_root = first_root,
        .revision = 1,
        .module_definitions = {
            module_definition(first_root, first_id, "iv.test.shared"),
        },
    }});
    auto const before = definitions.snapshot();

    auto result = publish(definitions, {
        iv::PackageRevision{
            .package_id = first_id,
            .package_root = first_root,
            .revision = 1,
            .module_definitions = {
                module_definition(first_root, first_id, "iv.test.shared"),
            },
        },
        iv::PackageRevision{
            .package_id = second_id,
            .package_root = second_root,
            .revision = 1,
            .module_definitions = {
                module_definition(second_root, second_id, "iv.test.shared"),
            },
        },
    }, 2);

    EXPECT_EQ(definitions.snapshot(), before);
    EXPECT_TRUE(result.publication_messages_by_package_id.contains(first_id));
    EXPECT_TRUE(result.publication_messages_by_package_id.contains(second_id));
    EXPECT_EQ(
        result.published_module_ids_by_package_id.at(first_id),
        std::vector<std::string>{"iv.test.shared"});
}

TEST(NodeDefinitions, CompleteAcceptedRevisionReplacesItsDefinitionSet)
{
    auto const workspace = fresh_module_fixture_workspace(
        "node_definitions_complete_revision_replace");
    auto const package_id = canonical_package_id(workspace);

    iv::NodeDefinitions definitions;
    publish(definitions, {iv::PackageRevision{
        .package_id = package_id,
        .package_root = workspace,
        .revision = 1,
        .module_definitions = {
            module_definition(workspace, package_id, "iv.test.a"),
            module_definition(workspace, package_id, "iv.test.b"),
        },
    }});
    ASSERT_EQ(definitions.loaded_module_definitions().size(), 2u);

    publish(definitions, {iv::PackageRevision{
        .package_id = package_id,
        .package_root = workspace,
        .revision = 2,
        .module_definitions = {
            module_definition(workspace, package_id, "iv.test.b"),
        },
    }}, 2);
    auto loaded = definitions.loaded_module_definitions();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().definition_id, "iv.test.b");
}

TEST(NodeDefinitions, PackageRemovalPublishesDefinitionDeletion)
{
    auto const workspace = fresh_module_fixture_workspace("node_definitions_removal");
    auto const package_id = canonical_package_id(workspace);

    iv::NodeDefinitions definitions;
    NodeDefinitionsWitness witness;
    node_definitions_witness_bridge::scope witness_scope{definitions, witness};
    publish(definitions, {iv::PackageRevision{
        .package_id = package_id,
        .package_root = workspace,
        .revision = 1,
        .module_definitions = {
            module_definition(workspace, package_id, "iv.test.removed"),
        },
    }});
    witness.package_definition_changes.clear();

    publish(definitions, {}, 2);
    EXPECT_TRUE(definitions.loaded_module_definitions().empty());
    ASSERT_EQ(witness.package_definition_changes.size(), 1u);
    EXPECT_EQ(
        witness.package_definition_changes.front().module_definitions.deleted_definition_ids,
        std::vector<std::string>{"iv.test.removed"});
}

TEST(NodeDefinitions, WithinPackageDuplicateDefinitionIdIsRejectedAtNamespaceBoundary)
{
    auto const workspace = fresh_module_fixture_workspace(
        "node_definitions_duplicate_within_package");
    auto const package_id = canonical_package_id(workspace);

    iv::NodeDefinitions definitions;
    auto result = publish(definitions, {iv::PackageRevision{
        .package_id = package_id,
        .package_root = workspace,
        .revision = 1,
        .module_definitions = {
            module_definition(workspace, package_id, "iv.test.duplicate"),
        },
        .leaf_definitions = {
            leaf_definition(workspace, package_id, "iv.test.duplicate"),
        },
    }});

    EXPECT_TRUE(definitions.snapshot()->by_id.empty());
    EXPECT_TRUE(result.publication_messages_by_package_id.contains(package_id));
}

TEST(NodeDefinitions, SeedLoadedDefinitionUsesAcceptedRevisionPublicationPath)
{
    auto const workspace = fresh_module_fixture_workspace("node_definitions_seed");
    iv::NodeDefinitions definitions;
    auto definition = make_loaded_definition(workspace, "iv.test.seeded");
    definitions.seed_loaded_definition(std::move(definition));

    auto const loaded = definitions.loaded_module_definitions();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().definition_id, "iv.test.seeded");
}

TEST(NodeDefinitions, NullPackageSnapshotIsRejectedWithoutMutatingPublishedNamespace)
{
    auto const workspace = fresh_module_fixture_workspace(
        "node_definitions_null_snapshot");
    auto const package_id = canonical_package_id(workspace);

    iv::NodeDefinitions definitions;
    publish(definitions, {iv::PackageRevision{
        .package_id = package_id,
        .package_root = workspace,
        .revision = 1,
        .module_definitions = {
            module_definition(workspace, package_id, "iv.test.stable"),
        },
    }});
    auto const before = definitions.snapshot();

    iv::PackageDefinitionsPublicationRequest request;
    EXPECT_THROW(
        definitions.handle_package_definitions_publication(request),
        std::invalid_argument);
    EXPECT_EQ(definitions.snapshot(), before);
}

TEST(NodeDefinitions, CrossPackageLeafModuleCollisionPreservesPreviousUnifiedNamespace)
{
    auto const module_root = fresh_module_fixture_workspace(
        "node_definitions_cross_kind_module");
    auto const leaf_root = fresh_module_fixture_workspace(
        "node_definitions_cross_kind_leaf");
    auto const module_package_id = canonical_package_id(module_root);
    auto const leaf_package_id = canonical_package_id(leaf_root);

    iv::NodeDefinitions definitions;
    publish(definitions, {iv::PackageRevision{
        .package_id = module_package_id,
        .package_root = module_root,
        .revision = 1,
        .module_definitions = {
            module_definition(module_root, module_package_id, "iv.test.shared.kind"),
        },
    }});
    auto const before = definitions.snapshot();

    auto result = publish(definitions, {
        iv::PackageRevision{
            .package_id = module_package_id,
            .package_root = module_root,
            .revision = 1,
            .module_definitions = {
                module_definition(module_root, module_package_id, "iv.test.shared.kind"),
            },
        },
        iv::PackageRevision{
            .package_id = leaf_package_id,
            .package_root = leaf_root,
            .revision = 1,
            .leaf_definitions = {
                leaf_definition(leaf_root, leaf_package_id, "iv.test.shared.kind"),
            },
        },
    }, 2);

    EXPECT_EQ(definitions.snapshot(), before);
    EXPECT_TRUE(result.publication_messages_by_package_id.contains(module_package_id));
    EXPECT_TRUE(result.publication_messages_by_package_id.contains(leaf_package_id));
    EXPECT_EQ(
        result.published_module_ids_by_package_id.at(module_package_id),
        std::vector<std::string>{"iv.test.shared.kind"});
    EXPECT_FALSE(result.published_leaf_ids_by_package_id.contains(leaf_package_id));
}
