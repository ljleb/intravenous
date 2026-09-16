#include "../module_test_utils.h"

#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/node_definitions_node_instances_bridge.h>
#include <intravenous/runtime/node_definitions_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/node_instances_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <gtest/gtest.h>



namespace {
using iv::test_support::fresh_module_fixture_workspace;
using iv::test_support::make_loaded_definition;
using iv::test_support::read_only_module_fixture_workspace;

}

TEST(IntrospectionBridges, DefinitionsToIvModuleSourceIntrospectionRequiresBinding)
{
    auto const workspace =
        fresh_module_fixture_workspace("runtime_bridges_defs_to_introspection_unbound");
    iv::NodeDefinitions definitions;
    iv::IvModuleSourceIntrospection introspection;

    definitions.seed_loaded_definition(make_loaded_definition(workspace));
    auto const result = introspection.query_active_regions(
        std::filesystem::weakly_canonical(workspace / "module.cpp"));
    EXPECT_TRUE(result.source_spans.empty());
}

TEST(IntrospectionBridges, DefinitionsToIvModuleSourceIntrospectionForwardsWhenBound)
{
    auto const workspace =
        read_only_module_fixture_workspace("local_cmake");
    iv::NodeDefinitions definitions;
    iv::IvModuleSourceIntrospection introspection;
    auto definitions_introspection_scope =
        iv::node_definitions_iv_module_source_introspection_bridge::bind(
            definitions,
            introspection);

    auto const startup = iv::StartupConfig(workspace, iv::test::repo_root(), {}).initialize();
    auto loaded = iv::test::load_runtime_iv_module_definition(
        startup,
        std::filesystem::weakly_canonical(workspace));
    definitions.seed_loaded_definition(iv::PackageModuleDefinition{
        .package_id = loaded.package_id,
        .definition_id = loaded.definition_id,
        .package_root = loaded.package_root,
        .module_id = loaded.module_id,
        .introspection = loaded.introspection,
        .dependencies = loaded.dependencies,
        .module_refs = std::move(loaded.module_refs),
        .root = loaded.root,
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
    iv::NodeDefinitions definitions;

    (void)instances.create_instance(
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
    auto bridge_scope =
        iv::node_definitions_node_instances_bridge::bind(
            definitions,
            instances);

    definitions.seed_loaded_definition(
        make_loaded_definition(package_root, std::string(module_id)));
    EXPECT_TRUE(instances.list_instances().empty());

    auto const instance_id = instances.create_instance(module_id, package_root);
    auto const listed = instances.list_instances();

    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed.front().instance_id, instance_id);
    EXPECT_EQ(listed.front().definition_id, module_id);
    EXPECT_TRUE(listed.front().realized);
}
