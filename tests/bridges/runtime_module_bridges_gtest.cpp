#include "../module_test_utils.h"

#include "runtime/iv_module_definitions.h"
#include "runtime/iv_module_definitions_iv_module_instances_bridge.h"
#include "runtime/iv_module_definitions_project_introspection_bridge.h"
#include "runtime/iv_module_instances.h"
#include "runtime/iv_module_instances_iv_module_definitions_bridge.h"
#include "runtime/project_introspection.h"
#include <gtest/gtest.h>

namespace {
using iv::test_support::fresh_test_workspace;
using iv::test_support::make_loaded_definition;
}

TEST(IntrospectionBridges, DefinitionsToProjectIntrospectionRequiresBinding)
{
    auto const workspace =
        fresh_test_workspace("runtime_bridges_defs_to_introspection_unbound");
    iv::IvModuleDefinitions definitions;
    iv::ProjectIntrospection introspection;

    definitions.seed_loaded_definition(make_loaded_definition(workspace));
    EXPECT_THROW((void)introspection.initialize(), std::runtime_error);
}

TEST(IntrospectionBridges, DefinitionsToProjectIntrospectionForwardsWhenBound)
{
    auto const workspace =
        fresh_test_workspace("runtime_bridges_defs_to_introspection_bound");
    iv::IvModuleDefinitions definitions;
    iv::ProjectIntrospection introspection;
    iv::bind_iv_module_definitions_project_introspection_bridge(introspection);

    auto const loaded = make_loaded_definition(workspace);
    auto const module_id = loaded.module_id;
    definitions.seed_loaded_definition(std::move(loaded));
    auto const initialized = introspection.initialize();

    EXPECT_EQ(initialized.module_id, module_id);

    iv::unbind_iv_module_definitions_project_introspection_bridge(introspection);
}

TEST(IntrospectionBridges, InstancesToDefinitionsRequiresBinding)
{
    auto const workspace =
        fresh_test_workspace("runtime_bridges_instances_to_definitions_unbound");

    iv::IvModuleInstances instances;
    iv::IvModuleDefinitions definitions;

    (void)instances.create_instance(std::filesystem::weakly_canonical(workspace));

    EXPECT_TRUE(definitions.loaded_definitions().empty());
}
