#include "../module_test_utils.h"

#include <intravenous/bridge.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/package_definitions_node_definitions_bridge.h>
#include <intravenous/runtime/package_definitions.h>
#include <intravenous/runtime/package_watcher.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
// The refresh transaction also carries PackageDefinitions' reply (the project
// publication), so the handler takes it as an lvalue.
void refresh_packages(
    iv::PackageDefinitions& packages,
    iv::PackageRefreshTransaction transaction)
{
    packages.handle_package_refresh(transaction);
}

constexpr std::string_view module_id = "iv.test.module";

// NodeInstances raises only the required-definitions event; its instance-list
// changes return in the ProjectGraph transaction record.
struct NodeInstancesWitness {
    std::optional<iv::IvModuleRequiredDefinitionsChanged> required_diff {};

    void reset()
    {
        required_diff.reset();
    }
    void handle_required_definitions_changed(
        iv::IvModuleRequiredDefinitionsChanged const &diff)
    {
        required_diff = diff;
    }
};

using namespace iv;
IV_DECLARE_BRIDGE(
    node_instances_witness_bridge,
    iv::NodeInstances,
    NodeInstancesWitness);
IV_DEFINE_BRIDGE(node_instances_witness_bridge)

iv::ModuleNodeDefinition make_definition(std::filesystem::path module_root)
{
    auto const normalized = std::filesystem::weakly_canonical(module_root).lexically_normal();
    return iv::ModuleNodeDefinition{
        .definition_id = std::string(module_id),
        .package_root = normalized,
        .module_id = "iv.test.module",
    };
}

void apply_module_definitions(
    iv::NodeInstances &instances,
    iv::ModuleNodeDefinitionsChanged diff)
{
    static std::uint64_t generation = 0;
    auto snapshot = std::make_shared<iv::NodeDefinitionsSnapshot>();
    snapshot->generation = ++generation;
    auto publish = [&](iv::ModuleNodeDefinition definition) {
        auto const id = definition.definition_id;
        snapshot->by_id.emplace(id, iv::NodeDefinitionEntry{
            .definition_id = id,
            .kind = iv::NodeDefinitionKind::module,
            .version = snapshot->generation,
            .definition = std::move(definition),
        });
    };
    for (auto& definition : diff.created) publish(std::move(definition));
    for (auto& definition : diff.updated) publish(std::move(definition));
    instances.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = std::move(snapshot)});
}

IV_SUBSCRIBE_LINKER_EVENT(
    node_instances_witness_bridge,
    iv_runtime_iv_module_required_definitions_changed_event,
    &NodeInstancesWitness::handle_required_definitions_changed)

class NodeInstancesTest : public ::testing::Test {
protected:
    iv::NodeInstances bridge_source {};
    NodeInstancesWitness witness {};
    node_instances_witness_bridge::scope witness_scope {bridge_source, witness};
};
} // namespace

TEST_F(NodeInstancesTest, CreateInstancePublishesRequiredDefinitionAndListsTheInstance)
{
    auto const workspace =
        iv::test_support::fresh_module_fixture_workspace("iv_module_instances_create");
    auto const module_root = std::filesystem::weakly_canonical(workspace);
    iv::NodeInstances instances;

    auto const instance_id = instances.create_instance(module_id, module_root);

    EXPECT_FALSE(instance_id.empty());
    ASSERT_TRUE(witness.required_diff.has_value());
    ASSERT_EQ(witness.required_diff->created.size(), 1u);
    EXPECT_EQ(witness.required_diff->created.front().definition_id, module_id);
    EXPECT_EQ(witness.required_diff->created.front().package_root, module_root);

    auto const listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed.front().instance_id, instance_id);
    EXPECT_EQ(listed.front().definition_id, module_id);
    EXPECT_FALSE(listed.front().realized);
}

TEST_F(NodeInstancesTest, CreateRejectsEmptyStableAndDefinitionIds)
{
    auto const workspace = iv::test_support::fresh_module_fixture_workspace(
        "node_instances_empty_ids");

    EXPECT_THROW(
        (void)bridge_source.create_instance(
            module_id,
            workspace,
            std::optional<std::string>{""}),
        std::invalid_argument);
    EXPECT_THROW(
        (void)bridge_source.create_instance("", workspace),
        std::invalid_argument);
    EXPECT_TRUE(bridge_source.desired_instances().empty());
}

TEST_F(NodeInstancesTest, CreateSecondInstanceForSameDefinitionDoesNotRepublishRequirement)
{
    auto const workspace =
        iv::test_support::fresh_module_fixture_workspace("iv_module_instances_dedup_required");
    auto const module_root = std::filesystem::weakly_canonical(workspace);
    iv::NodeInstances instances;

    (void)instances.create_instance(module_id, module_root);
    witness.reset();

    auto const second_instance_id = instances.create_instance(module_id, module_root);

    EXPECT_FALSE(second_instance_id.empty());
    EXPECT_FALSE(witness.required_diff.has_value());
    EXPECT_EQ(instances.list_instances().size(), 2u);
}

TEST_F(NodeInstancesTest, SameDefinitionIdAtNewRootRepublishesUpdatedRequirement)
{
    auto const first_workspace =
        iv::test_support::fresh_module_fixture_workspace("iv_module_instances_move_root_a");
    auto const second_workspace =
        iv::test_support::fresh_module_fixture_workspace("iv_module_instances_move_root_b");
    auto const first_root = std::filesystem::weakly_canonical(first_workspace);
    auto const second_root = std::filesystem::weakly_canonical(second_workspace);
    iv::NodeInstances instances;

    (void)instances.create_instance(module_id, first_root);
    witness.reset();

    auto const second_instance_id = instances.create_instance(module_id, second_root);

    EXPECT_FALSE(second_instance_id.empty());
    ASSERT_TRUE(witness.required_diff.has_value());
    ASSERT_EQ(witness.required_diff->updated.size(), 1u);
    EXPECT_EQ(witness.required_diff->updated.front().definition_id, module_id);
    EXPECT_EQ(witness.required_diff->updated.front().package_root, second_root);

    auto const listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 2u);
    EXPECT_EQ(listed[0].definition_id, module_id);
    EXPECT_EQ(listed[1].definition_id, module_id);
}

TEST_F(NodeInstancesTest, DefinitionChangeMovesInstanceToPublishedPackageRoot)
{
    auto const workspace =
        iv::test_support::fresh_module_fixture_workspace("iv_module_instances_move_published_root");
    auto const stale_root = std::filesystem::weakly_canonical(workspace / "stale");
    auto const moved_root = std::filesystem::weakly_canonical(workspace);
    iv::NodeInstances instances;

    (void)instances.create_instance(module_id, stale_root);
    witness.reset();

    apply_module_definitions(instances, iv::ModuleNodeDefinitionsChanged{
        .created = {make_definition(moved_root)},
    });

    // A definition publication is already authoritative about package ownership.
    // Updating the local instance snapshot must not feed a second requirement event
    // back into NodeDefinitions during the same source-event propagation.
    EXPECT_FALSE(witness.required_diff.has_value());
    auto const listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed.front().package_root, moved_root);
    EXPECT_TRUE(listed.front().realized);
}

TEST_F(NodeInstancesTest, PackageRegistryListsQueuedPackagesBeforeTheirFirstBuild)
{
    auto const workspace =
        iv::test_support::fresh_module_fixture_workspace("iv_module_instances_many_source_modules");
    auto const source_root = workspace / "modules" / "many";
    std::filesystem::create_directories(source_root);
    iv::test_support::write_text(
        source_root / "iv_package.json",
        "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
    iv::test_support::write_text(
        source_root / "module.cpp",
        "#include <intravenous/dsl.h>\n\n"
        "void primary(iv::GraphBuilder& g) { g.outputs(); }\n"
        "void secondary(iv::GraphBuilder& g) { g.outputs(); }\n\n"
        "IV_MODULE(\"iv.test.module\", primary);\n"
        "IV_MODULE(\"iv.test.module.secondary\", secondary);\n");

    iv::NodeDefinitions definitions;
    iv::PackageDefinitions sources(workspace);
    auto package_definitions_scope =
        iv::package_definitions_node_definitions_bridge::bind(sources, definitions);
    auto declarations = iv::discover_iv_package_declarations(workspace, {});
    refresh_packages(sources, iv::PackageRefreshTransaction{
        .declarations = iv::IvPackageDeclarationsChanged{
            .created = std::move(declarations),
        },
    });
    auto const discovered = sources.list_packages();

    ASSERT_EQ(discovered.size(), 1u);
    auto const expected_root = std::filesystem::weakly_canonical(source_root);
    EXPECT_EQ(discovered.front().package_root, expected_root);
    EXPECT_EQ(discovered.front().package_id, expected_root.generic_string());
    EXPECT_EQ(discovered.front().build_state, iv::PackageBuildState::queued);
    EXPECT_TRUE(discovered.front().module_ids.empty());
}

TEST_F(NodeInstancesTest, DefinitionsChangedRealizesMatchingInstances)
{
    auto const workspace =
        iv::test_support::fresh_module_fixture_workspace("iv_module_instances_realize");
    auto const module_root = std::filesystem::weakly_canonical(workspace);
    iv::NodeInstances instances;

    auto const instance_id = instances.create_instance(module_id, module_root);
    witness.reset();

    apply_module_definitions(instances, iv::ModuleNodeDefinitionsChanged{
        .created = {make_definition(module_root)},
    });

    auto const listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed.front().instance_id, instance_id);
    EXPECT_EQ(listed.front().definition_id, module_id);
    EXPECT_TRUE(listed.front().realized);
    EXPECT_EQ(listed.front().module_id, "iv.test.module");
}

TEST_F(NodeInstancesTest, DefinitionRemovalKeepsDesiredInstanceVisibleAsUnrealized)
{
    auto const workspace =
        iv::test_support::fresh_module_fixture_workspace("iv_module_instances_removed_definition");
    auto const module_root = std::filesystem::weakly_canonical(workspace);
    iv::NodeInstances instances;

    auto const instance_id = instances.create_instance(module_id, module_root);
    apply_module_definitions(instances, iv::ModuleNodeDefinitionsChanged{
        .created = {make_definition(module_root)},
    });
    witness.reset();

    // A successful package revision may intentionally remove a definition even
    // while the project still desires an instance of it. Drop the published
    // snapshot but preserve durable instance metadata so the UI can show it as
    // unresolved and offer deletion instead of hiding orphaned project state.
    apply_module_definitions(instances, iv::ModuleNodeDefinitionsChanged{
        .deleted_definition_ids = {std::string(module_id)},
    });

    auto const listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed.front().instance_id, instance_id);
    EXPECT_EQ(listed.front().definition_id, module_id);
    EXPECT_FALSE(listed.front().realized);
}

TEST_F(NodeInstancesTest, RemoveLastPublishedInstanceUnlistsItAndDropsRequirement)
{
    auto const workspace =
        iv::test_support::fresh_module_fixture_workspace("iv_module_instances_remove");
    auto const module_root = std::filesystem::weakly_canonical(workspace);
    iv::NodeInstances instances;

    auto const instance_id = instances.create_instance(module_id, module_root);
    apply_module_definitions(instances, iv::ModuleNodeDefinitionsChanged{
        .created = {make_definition(module_root)},
    });
    witness.reset();

    instances.remove_instance(instance_id);

    ASSERT_TRUE(witness.required_diff.has_value());
    ASSERT_EQ(witness.required_diff->deleted_definition_ids.size(), 1u);
    EXPECT_EQ(witness.required_diff->deleted_definition_ids.front(), module_id);
    EXPECT_TRUE(instances.list_instances().empty());
}

TEST_F(NodeInstancesTest, ConfigurationSourceUpdateListsTheNewDurableRequest)
{
    auto const workspace = iv::test_support::fresh_module_fixture_workspace(
        "iv_module_instances_configuration_source_update");
    auto const module_root = std::filesystem::weakly_canonical(workspace);
    auto const instance_id = bridge_source.create_instance(
        module_id,
        module_root,
        "instance:configured",
        std::nullopt,
        "440");
    apply_module_definitions(bridge_source, iv::ModuleNodeDefinitionsChanged{
        .created = {make_definition(module_root)},
    });
    witness.reset();

    bridge_source.update_instances({iv::NodeInstances::Update{
        .instance_id = instance_id,
        .configuration_source = "880",
    }});

    auto const listed = bridge_source.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed.front().configuration_source, "880");
}

TEST_F(NodeInstancesTest, TransactionReturnsItsInstanceReadModelChanges)
{
    auto const workspace = iv::test_support::fresh_module_fixture_workspace(
        "node_instances_transaction_changes");
    auto const module_root = std::filesystem::weakly_canonical(workspace);

    iv::GraphBuilder root;
    iv::NodeInstancesProjectGraphRequest created{
        .snapshot = std::make_shared<iv::NodeDefinitionsSnapshot const>(),
        .root_builder = &root,
        .mutation = iv::NodeInstanceCreateMutation{
            .instance_id = "instance:created",
            .definition_id = std::string(module_id),
            .package_root = module_root,
        },
    };
    bridge_source.handle_project_graph_transaction(created);

    // NodeInstances publishes nothing itself; ProjectGraph publishes these.
    EXPECT_TRUE(created.instances_list_changed);
    EXPECT_TRUE(created.instance_declarations_changed);
    ASSERT_EQ(created.instances.size(), 1u);
    EXPECT_EQ(created.instances.front().instance_id, "instance:created");
    EXPECT_FALSE(created.instances.front().realized);

    iv::GraphBuilder unchanged_root;
    iv::NodeInstancesProjectGraphRequest unchanged{
        .snapshot = created.snapshot,
        .root_builder = &unchanged_root,
    };
    bridge_source.handle_project_graph_transaction(unchanged);

    EXPECT_FALSE(unchanged.instances_list_changed);
    EXPECT_FALSE(unchanged.instance_declarations_changed);
    EXPECT_TRUE(unchanged.instances.empty());
}

TEST_F(NodeInstancesTest, InvalidReplaceAllDoesNotPartiallyChangeDesiredState)
{
    auto const workspace = iv::test_support::fresh_module_fixture_workspace(
        "node_instances_replace_all_atomicity");
    auto const module_root = std::filesystem::weakly_canonical(workspace);
    (void)bridge_source.create_instance(
        module_id,
        module_root,
        "instance:existing",
        "Existing",
        "440");
    auto const before = bridge_source.desired_instances();
    witness.reset();

    iv::GraphBuilder root;
    iv::NodeInstancesProjectGraphRequest request{
        .snapshot = std::make_shared<iv::NodeDefinitionsSnapshot const>(),
        .root_builder = &root,
        .mutation = iv::NodeInstanceReplaceAllMutation{
            .instances = {
                {
                    .instance_id = "instance:duplicate",
                    .definition_id = "iv.test.first",
                    .package_root = module_root,
                },
                {
                    .instance_id = "instance:duplicate",
                    .definition_id = "iv.test.second",
                    .package_root = module_root,
                },
            },
        },
    };

    EXPECT_THROW(
        bridge_source.handle_project_graph_transaction(request),
        std::invalid_argument);
    EXPECT_TRUE(request.handled);
    EXPECT_EQ(bridge_source.desired_instances(), before);
    EXPECT_FALSE(witness.required_diff.has_value());
    EXPECT_FALSE(request.instances_list_changed);
    EXPECT_FALSE(request.instance_declarations_changed);
}

TEST_F(NodeInstancesTest, ReplaceAllDoesNotInventARootForAnUnresolvedDefinition)
{
    iv::GraphBuilder root;
    iv::NodeInstancesProjectGraphRequest request{
        .snapshot = std::make_shared<iv::NodeDefinitionsSnapshot const>(),
        .root_builder = &root,
        .mutation = iv::NodeInstanceReplaceAllMutation{
            .instances = {{
                .instance_id = "instance:unresolved",
                .definition_id = "iv.test.unresolved",
                .configuration_source = "440",
            }},
        },
    };

    bridge_source.handle_project_graph_transaction(request);

    ASSERT_TRUE(request.handled);
    ASSERT_EQ(request.result.diagnostics.size(), 1u);
    auto const declarations = bridge_source.desired_instances();
    ASSERT_EQ(declarations.size(), 1u);
    EXPECT_EQ(declarations.front().instance_id, "instance:unresolved");
    EXPECT_FALSE(declarations.front().package_root.has_value());
    EXPECT_FALSE(witness.required_diff.has_value());
}
