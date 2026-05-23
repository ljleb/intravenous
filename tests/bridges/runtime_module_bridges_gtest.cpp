#include "../module_test_utils.h"

#include "runtime/iv_module_definitions.h"
#include "runtime/iv_module_definitions_iv_module_instances_bridge.h"
#include "runtime/iv_module_definitions_project_introspection_bridge.h"
#include "runtime/iv_module_instances.h"
#include "runtime/iv_module_instances_iv_module_definitions_bridge.h"
#include "runtime/project_introspection.h"
#include "runtime/startup_config.h"

#include <gtest/gtest.h>

namespace {
using iv::test_support::copy_fixture_workspace;
}

TEST(RuntimeBridges, DefinitionsToProjectIntrospectionRequiresBinding)
{
    auto const workspace =
        copy_fixture_workspace("runtime_bridges_defs_to_introspection_unbound", "local_cmake");
    iv::test_support::write_text(workspace / ".intravenous", "");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::RuntimeIvModuleDefinitions definitions;
    iv::RuntimeProjectIntrospection introspection;

    definitions.seed_loaded_definition(
        iv::test::load_runtime_iv_module_definition(
            startup,
            std::filesystem::weakly_canonical(workspace)));
    EXPECT_THROW((void)introspection.initialize(), std::runtime_error);
}

TEST(RuntimeBridges, DefinitionsToProjectIntrospectionForwardsWhenBound)
{
    auto const workspace =
        copy_fixture_workspace("runtime_bridges_defs_to_introspection_bound", "local_cmake");
    iv::test_support::write_text(workspace / ".intravenous", "");

    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    auto const startup = startup_config.initialize();
    iv::RuntimeIvModuleDefinitions definitions;
    iv::RuntimeProjectIntrospection introspection;
    iv::bind_iv_module_definitions_project_introspection_bridge(introspection);

    auto const loaded = iv::test::load_runtime_iv_module_definition(
        startup,
        std::filesystem::weakly_canonical(workspace));
    auto const module_id = loaded.module_id;
    definitions.seed_loaded_definition(std::move(loaded));
    auto const initialized = introspection.initialize();

    EXPECT_EQ(initialized.module_id, module_id);

    iv::unbind_iv_module_definitions_project_introspection_bridge(introspection);
}

TEST(RuntimeBridges, InstancesToDefinitionsRequiresBinding)
{
    auto const workspace =
        copy_fixture_workspace("runtime_bridges_instances_to_definitions_unbound", "local_cmake");
    iv::test_support::write_text(workspace / ".intravenous", "");

    iv::RuntimeIvModuleInstances instances;
    iv::RuntimeIvModuleDefinitions definitions;

    (void)instances.create_instance(std::filesystem::weakly_canonical(workspace));

    EXPECT_TRUE(definitions.loaded_definitions().empty());
}
