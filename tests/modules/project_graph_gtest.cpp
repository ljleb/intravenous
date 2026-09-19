#include <intravenous/dsl.h>
#include <intravenous/module/package_definitions.h>
#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/node_definitions_project_graph_bridge.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/package_pipeline_types.h>
#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/project_graph_graph_connections_bridge.h>
#include <intravenous/runtime/project_graph_node_instances_bridge.h>
#include <intravenous/runtime/runtime_project_events.h>

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace {
using iv::details::ConfigurationArgument;
using iv::details::PackageDefinition;
using iv::details::PackageDefinitionKind;
using iv::details::RegisteredSignature;

constexpr char definition_id[] = "iv.test.project_graph.node";
constexpr char package_root[] = "/tmp/iv-project-graph-tests";
constexpr char source_file[] = "/tmp/iv-project-graph-tests/module.cpp";
RegisteredSignature const zero_signature{};
std::atomic<int> configure_calls{0};

RegisteredSignature const* signature_callback() { return &zero_signature; }

void configure_module(iv::GraphBuilder& graph, std::span<ConfigurationArgument>)
{
    using namespace iv;
    ++configure_calls;
    auto input = graph.input<"main">();
    graph.outputs("main"_P = input);
}

std::shared_ptr<iv::NodeDefinitionsSnapshot const> make_snapshot(std::uint64_t generation)
{
    auto revision = std::make_shared<iv::PackageRevision>();
    revision->package_id = "iv.test.project_graph.package";
    revision->package_root = package_root;
    revision->revision = generation;
    revision->provider_definitions.push_back(PackageDefinition{
        .kind = PackageDefinitionKind::module,
        .id = definition_id,
        .id_size = sizeof(definition_id) - 1,
        .source_file = source_file,
        .source_file_size = sizeof(source_file) - 1,
        .package_root = package_root,
        .package_root_size = sizeof(package_root) - 1,
        .module_build = &configure_module,
        .signature = &signature_callback,
    });

    auto snapshot = std::make_shared<iv::NodeDefinitionsSnapshot>();
    snapshot->generation = generation;
    snapshot->package_revisions.push_back(revision);
    snapshot->by_id.emplace(definition_id, iv::NodeDefinitionEntry{
        .definition_id = definition_id,
        .kind = iv::NodeDefinitionKind::module,
        .version = generation,
        .definition = iv::ModuleNodeDefinition{
            .definition_id = definition_id,
            .package_id = revision->package_id,
            .package_root = revision->package_root,
            .module_id = definition_id,
            .provider = iv::NodeDefinitionProvider{
                .module_build = &configure_module,
                .signature = &signature_callback,
            },
        },
    });
    return snapshot;
}

struct ProjectGraphFixture : ::testing::Test {
    iv::NodeInstances instances;
    iv::GraphConnections connections;
    iv::ProjectGraph project_graph;
    iv::project_graph_node_instances_bridge::scope instances_scope{
        project_graph, instances};
    iv::project_graph_graph_connections_bridge::scope connections_scope{
        project_graph, connections};

    void SetUp() override { configure_calls = 0; }
};
} // namespace

TEST_F(ProjectGraphFixture, DefinitionAndMutationTransactionsBuildWholeRootGenerations)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(1)});
    auto generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->generation, 1u);
    EXPECT_EQ(generation->definitions_generation, 1u);
    EXPECT_TRUE(generation->placements.empty());

    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_iv_module_instance(
        iv::ProjectCreateIvModuleInstanceRequest{
            .instance_id = "instance:a",
            .module_id = definition_id,
        },
        create_builder);
    EXPECT_EQ(create_builder.build(), "instance:a");

    generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->generation, 2u);
    ASSERT_EQ(generation->placements.size(), 1u);
    EXPECT_TRUE(generation->placements.contains("instance:a"));
    EXPECT_TRUE(generation->diagnostics.empty());
    EXPECT_EQ(configure_calls.load(), 1);

    iv::ProjectAckBuilder update_builder;
    project_graph.handle_project_update_iv_module_instances(
        iv::ProjectUpdateIvModuleInstancesRequest{
            .updates = {{.instance_id = "instance:a", .display_name = "Renamed"}},
        },
        update_builder);
    EXPECT_NO_THROW(update_builder.build());
    generation = project_graph.current_generation();
    EXPECT_EQ(generation->generation, 3u);
    EXPECT_EQ(generation->placements.size(), 1u);
    // Same definitions generation + same typed value tuple reuses configuration.
    EXPECT_EQ(configure_calls.load(), 1);

    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(2)});
    generation = project_graph.current_generation();
    EXPECT_EQ(generation->generation, 4u);
    EXPECT_EQ(generation->definitions_generation, 2u);
    EXPECT_EQ(generation->placements.size(), 1u);
    EXPECT_EQ(configure_calls.load(), 2);

    iv::ProjectAckBuilder delete_builder;
    project_graph.handle_project_delete_iv_module_instance(
        iv::ProjectDeleteIvModuleInstanceRequest{.instance_id = "instance:a"},
        delete_builder);
    EXPECT_NO_THROW(delete_builder.build());
    generation = project_graph.current_generation();
    EXPECT_EQ(generation->generation, 5u);
    EXPECT_TRUE(generation->placements.empty());
    EXPECT_TRUE(instances.list_instances().empty());
}

TEST_F(ProjectGraphFixture, MissingDefinitionRemainsDesiredAndRealizesWhenDefinitionArrives)
{
    iv::ProjectStringBuilder builder;
    project_graph.handle_project_create_iv_module_instance(
        iv::ProjectCreateIvModuleInstanceRequest{
            .instance_id = "instance:pending",
            .module_id = definition_id,
            .package_root = std::filesystem::path(package_root),
        },
        builder);
    EXPECT_EQ(builder.build(), "instance:pending");

    auto generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_TRUE(generation->placements.empty());
    ASSERT_EQ(generation->diagnostics.size(), 1u);
    EXPECT_EQ(generation->diagnostics.front().instance_id, "instance:pending");
    auto listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_FALSE(listed.front().realized);

    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(1)});
    generation = project_graph.current_generation();
    ASSERT_TRUE(generation->placements.contains("instance:pending"));
    EXPECT_TRUE(generation->diagnostics.empty());
    listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_TRUE(listed.front().realized);
}

TEST_F(ProjectGraphFixture, StaleDefinitionsSnapshotCannotRollBackRootGeneration)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(2)});
    auto const current = project_graph.current_generation();
    ASSERT_TRUE(current);

    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(1)});
    auto const after_stale = project_graph.current_generation();
    ASSERT_TRUE(after_stale);
    EXPECT_EQ(after_stale->generation, current->generation);
    EXPECT_EQ(after_stale->definitions_generation, 2u);
}

TEST(ProjectGraphBridge, NodeDefinitionsSnapshotFlowsThroughProjectGraphBeforeNodeInstances)
{
    iv::NodeDefinitions definitions;
    iv::ProjectGraph project_graph;
    iv::NodeInstances instances;
    iv::GraphConnections connections;
    auto definitions_scope = iv::node_definitions_project_graph_bridge::bind(
        definitions, project_graph);
    auto instances_scope = iv::project_graph_node_instances_bridge::bind(
        project_graph, instances);
    auto connections_scope = iv::project_graph_graph_connections_bridge::bind(
        project_graph, connections);

    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_node_definitions_snapshot_changed_event,
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(7)});

    auto const generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->definitions_generation, 7u);
    EXPECT_EQ(instances.configuration_cache_size(), 0u);
}

TEST(ProjectGraph, FailedRootBuildDoesNotCommitDefinitionsAndSameSnapshotCanRetry)
{
    iv::ProjectGraph project_graph;
    iv::NodeInstances instances;
    auto instances_scope = iv::project_graph_node_instances_bridge::bind(
        project_graph, instances);
    auto snapshot = make_snapshot(1);
    EXPECT_THROW(
        project_graph.handle_node_definitions_snapshot_changed(
            iv::NodeDefinitionsSnapshotChanged{.snapshot = snapshot}),
        std::runtime_error);
    EXPECT_FALSE(project_graph.current_generation());
    EXPECT_EQ(project_graph.definitions_snapshot()->generation, 0u);

    iv::GraphConnections connections;
    auto connections_scope = iv::project_graph_graph_connections_bridge::bind(
        project_graph, connections);
    EXPECT_NO_THROW(project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = snapshot}));
    auto generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->definitions_generation, 1u);
}

TEST_F(ProjectGraphFixture, InvalidUpdateBatchDoesNotPartiallyMutateDesiredState)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(1)});
    iv::ProjectStringBuilder create_builder;
    project_graph.handle_project_create_iv_module_instance(
        iv::ProjectCreateIvModuleInstanceRequest{
            .instance_id = "instance:a",
            .module_id = definition_id,
            .display_name = "Original",
        },
        create_builder);
    (void)create_builder.build();
    auto const before = project_graph.current_generation();
    ASSERT_TRUE(before);

    iv::ProjectAckBuilder update_builder;
    EXPECT_THROW(
        project_graph.handle_project_update_iv_module_instances(
            iv::ProjectUpdateIvModuleInstancesRequest{
                .updates = {
                    {.instance_id = "instance:a", .display_name = "Changed"},
                    {.instance_id = "instance:missing", .display_name = "Invalid"},
                },
            },
            update_builder),
        std::runtime_error);

    auto listed = instances.list_instances();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed.front().display_name, "Original");
    EXPECT_EQ(project_graph.current_generation()->generation, before->generation);
}


TEST_F(ProjectGraphFixture, ConnectionMutationRebuildsWholeRootAndDanglingIntentRecovers)
{
    project_graph.handle_node_definitions_snapshot_changed(
        iv::NodeDefinitionsSnapshotChanged{.snapshot = make_snapshot(1)});

    for (auto const* instance_id : {"instance:source", "instance:sink"}) {
        iv::ProjectStringBuilder builder;
        project_graph.handle_project_create_iv_module_instance(
            iv::ProjectCreateIvModuleInstanceRequest{
                .instance_id = std::string(instance_id),
                .module_id = definition_id,
            },
            builder);
        EXPECT_EQ(builder.build(), instance_id);
    }

    iv::ProjectAckBuilder connect_builder;
    project_graph.handle_project_upsert_graph_connection(
        iv::ProjectUpsertGraphConnectionRequest{
            .connection = iv::ProjectSampleConnection{
                .connection_id = "connection:main",
                .outputs = {{
                    .instance_id = "instance:source",
                    .port = {.name = "main"},
                }},
                .inputs = {{
                    .instance_id = "instance:sink",
                    .port = {.name = "main"},
                }},
            },
        },
        connect_builder);
    EXPECT_NO_THROW(connect_builder.build());
    auto generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->applied_connection_ids,
              std::vector<std::string>{"connection:main"});
    EXPECT_TRUE(generation->connection_diagnostics.empty());
    ASSERT_EQ(connections.desired_connections().size(), 1u);

    iv::ProjectAckBuilder delete_builder;
    project_graph.handle_project_delete_iv_module_instance(
        iv::ProjectDeleteIvModuleInstanceRequest{.instance_id = "instance:sink"},
        delete_builder);
    EXPECT_NO_THROW(delete_builder.build());
    generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_TRUE(generation->applied_connection_ids.empty());
    ASSERT_EQ(generation->connection_diagnostics.size(), 1u);
    EXPECT_EQ(connections.desired_connections().size(), 1u);

    iv::ProjectStringBuilder recreate_builder;
    project_graph.handle_project_create_iv_module_instance(
        iv::ProjectCreateIvModuleInstanceRequest{
            .instance_id = "instance:sink",
            .module_id = definition_id,
        },
        recreate_builder);
    EXPECT_EQ(recreate_builder.build(), "instance:sink");
    generation = project_graph.current_generation();
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation->applied_connection_ids,
              std::vector<std::string>{"connection:main"});
    EXPECT_TRUE(generation->connection_diagnostics.empty());
}
