#include "../module_test_utils.h"
#include "../event_source_test_support.h"
#include "../node_definitions_test_support.h"
#include "../node_instances_test_driver.h"
#include <intravenous/runtime/runtime_project_events.h>

#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/node_definitions_project_graph_bridge.h>
#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/project_graph_graph_connections_bridge.h>
#include <intravenous/runtime/project_graph_node_instances_bridge.h>
#include <intravenous/runtime/project_graph_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/node_instances_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <gtest/gtest.h>



namespace {
using iv::test_support::fresh_module_fixture_workspace;
using iv::test_support::make_loaded_definition;
using iv::test_support::read_only_module_fixture_workspace;

}

TEST(IntrospectionBridges, DefinitionsReachIvModuleSourceIntrospectionOnlyThroughProjectGraph)
{
    auto const workspace =
        fresh_module_fixture_workspace("runtime_bridges_defs_to_introspection_unbound");
    iv::NodeDefinitions definitions;
    iv::IvModuleSourceIntrospection introspection;

    iv::test::publish_loaded_definition(definitions, make_loaded_definition(workspace));
    auto const result = introspection.query_active_regions(
        std::filesystem::weakly_canonical(workspace / "module.cpp"));
    EXPECT_TRUE(result.source_spans.empty());
}

// ProjectGraph applies a definitions publication to source introspection once,
// in the root build the publication causes.
TEST(IntrospectionBridges, DefinitionsReachIvModuleSourceIntrospectionThroughProjectGraph)
{
    auto const workspace =
        read_only_module_fixture_workspace("local_cmake");
    iv::NodeDefinitions definitions;
    iv::NodeInstances instances;
    iv::GraphConnections graph_connections;
    iv::ProjectGraph project_graph;
    iv::IvModuleSourceIntrospection introspection;
    auto definitions_project_graph_scope =
        iv::node_definitions_project_graph_bridge::bind(definitions, project_graph);
    auto project_graph_instances_scope =
        iv::project_graph_node_instances_bridge::bind(project_graph, instances);
    auto project_graph_connections_scope =
        iv::project_graph_graph_connections_bridge::bind(project_graph, graph_connections);
    auto project_graph_introspection_scope =
        iv::project_graph_iv_module_source_introspection_bridge::bind(
            project_graph, introspection);

    auto const startup = iv::StartupConfig(workspace, iv::test::repo_root(), {}).initialize();
    auto loaded = iv::test::load_runtime_iv_module_definition(
        startup,
        std::filesystem::weakly_canonical(workspace));
    iv::test::publish_loaded_definition(definitions, iv::PackageModuleDefinition{
        .package_id = loaded.package_id,
        .definition_id = loaded.definition_id,
        .package_root = loaded.package_root,
        .module_id = loaded.module_id,
        .introspection = loaded.introspection,
        .dependencies = loaded.dependencies,
        .module_refs = std::move(loaded.module_refs),
        .configured_graph = loaded.configured_graph,
    });
    auto const result = introspection.query_active_regions(
        std::filesystem::weakly_canonical(workspace / "module.cpp"));

    EXPECT_FALSE(result.source_spans.empty());
}

TEST(IntrospectionBridges, InstancesToDefinitionsRequiresBinding)
{
    auto const workspace =
        fresh_module_fixture_workspace("runtime_bridges_instances_to_definitions_unbound");

    iv::NodeInstances instances;
    iv::test::NodeInstancesDriver driver {instances};
    iv::NodeDefinitions definitions;

    (void)driver.create(
        "iv.test.runtime_module_bridges",
        std::filesystem::weakly_canonical(workspace));

    EXPECT_TRUE(definitions.loaded_module_definitions().empty());
}

TEST(InstanceDefinitionBridges, InstanceCreatedAfterDefinitionPublicationRealizesImmediately)
{
    auto const workspace =
        fresh_module_fixture_workspace("runtime_bridges_definition_before_instance");
    auto const package_root = std::filesystem::weakly_canonical(workspace);
    constexpr std::string_view module_id = "iv.test.definition_before_instance";

    iv::NodeInstances instances;
    iv::NodeDefinitions definitions;
    iv::GraphConnections graph_connections;
    iv::ProjectGraph project_graph;
    auto definitions_project_graph_scope =
        iv::node_definitions_project_graph_bridge::bind(
            definitions, project_graph);
    auto project_graph_instances_scope =
        iv::project_graph_node_instances_bridge::bind(
            project_graph, instances);
    auto project_graph_connections_scope =
        iv::project_graph_graph_connections_bridge::bind(
            project_graph, graph_connections);

    iv::test::publish_loaded_definition(definitions, make_loaded_definition(package_root, std::string(module_id)));
    EXPECT_TRUE(instances.list_instances().empty());

    iv::ProjectStringBuilder create_builder;
    in_event_source([&] {
        project_graph.handle_project_create_node_instance(
            iv::ProjectCreateNodeInstanceRequest{
                .definition_id = std::string(module_id),
                .package_root = package_root,
            },
            create_builder);
    });
    auto const instance_id = create_builder.build();
    auto const listed = instances.list_instances();

    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed.front().instance_id, instance_id);
    EXPECT_EQ(listed.front().definition_id, module_id);
    EXPECT_TRUE(listed.front().realized);
}
