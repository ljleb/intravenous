#include "../module_test_utils.h"

#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_instances_bridge.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_instances_iv_module_definitions_bridge.h>
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
    iv::IvModuleDefinitions definitions;
    iv::IvModuleSourceIntrospection introspection;

    definitions.seed_loaded_definition(make_loaded_definition(workspace));
    auto const result = introspection.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {
            iv::SourceRange{
                .start = {.line = 1, .column = 1},
                .end = {.line = 2, .column = 1},
            },
        });
    EXPECT_TRUE(result.nodes.empty());
}

TEST(IntrospectionBridges, DefinitionsToIvModuleSourceIntrospectionForwardsWhenBound)
{
    auto const workspace =
        read_only_module_fixture_workspace("local_cmake");
    iv::IvModuleDefinitions definitions;
    iv::IvModuleSourceIntrospection introspection;
    iv::bind_iv_module_definitions_iv_module_source_introspection_bridge(introspection);

    auto const startup = iv::StartupConfig(workspace, iv::test::repo_root(), {}).initialize();
    auto loaded = iv::test::load_runtime_iv_module_definition(
        startup,
        std::filesystem::weakly_canonical(workspace));
    definitions.seed_loaded_definition(iv::IvModuleReloadedDefinition{
        .definition_id = loaded.definition_id,
        .module_root = loaded.module_root,
        .module_id = loaded.module_id,
        .introspection = loaded.introspection,
        .dependencies = loaded.dependencies,
        .module_refs = std::move(loaded.module_refs),
        .canonical_builder = std::move(loaded.canonical_builder),
    });
    auto const result = introspection.query_active_regions(
        std::filesystem::weakly_canonical(workspace / "module.cpp"));

    EXPECT_FALSE(result.source_spans.empty());

    iv::unbind_iv_module_definitions_iv_module_source_introspection_bridge(introspection);
}

TEST(IntrospectionBridges, InstancesToDefinitionsRequiresBinding)
{
    auto const workspace =
        fresh_module_fixture_workspace("runtime_bridges_instances_to_definitions_unbound");

    iv::IvModuleInstances instances;
    iv::IvModuleDefinitions definitions;

    (void)instances.create_instance(std::filesystem::weakly_canonical(workspace));

    EXPECT_TRUE(definitions.loaded_definitions().empty());
}
