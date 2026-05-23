#include "runtime_test_harness.h"

#include "runtime/graph_input_lanes.h"
#include "runtime/graph_input_lanes_timeline_bridge.h"
#include "runtime/iv_module_definitions.h"
#include "runtime/iv_module_definitions_iv_module_instances_bridge.h"
#include "runtime/iv_module_definitions_project_introspection_bridge.h"
#include "runtime/iv_module_instances.h"
#include "runtime/iv_module_instances_graph_input_lanes_bridge.h"
#include "runtime/project_introspection.h"
#include "runtime/timeline.h"

#include <gtest/gtest.h>

namespace {
using iv::test_support::copy_fixture_workspace;
}

TEST(RuntimeBridges, DefinitionsToProjectIntrospectionRequiresBinding)
{
    auto const workspace =
        copy_fixture_workspace("runtime_bridges_defs_to_introspection_unbound", "local_cmake");
    iv::test_support::write_text(workspace / ".intravenous", "");

    iv::RuntimeIvModuleDefinitions definitions(workspace, iv::test::repo_root(), {}, {});
    iv::RuntimeProjectIntrospection introspection;

    (void)definitions.initialize();
    EXPECT_THROW((void)introspection.initialize(), std::runtime_error);

    definitions.request_shutdown();
}

TEST(RuntimeBridges, DefinitionsToProjectIntrospectionForwardsWhenBound)
{
    auto const workspace =
        copy_fixture_workspace("runtime_bridges_defs_to_introspection_bound", "local_cmake");
    iv::test_support::write_text(workspace / ".intravenous", "");

    iv::RuntimeIvModuleDefinitions definitions(workspace, iv::test::repo_root(), {}, {});
    iv::RuntimeProjectIntrospection introspection;
    iv::bind_iv_module_definitions_project_introspection_bridge(introspection);

    auto const definition = definitions.initialize();
    auto const initialized = introspection.initialize();

    EXPECT_EQ(initialized.module_id, definition.module_id);

    iv::unbind_iv_module_definitions_project_introspection_bridge(introspection);
    definitions.request_shutdown();
}

TEST(RuntimeBridges, DefinitionsInstancesAndGraphInputLanesRequireBindingsForLaneQueries)
{
    auto const workspace =
        copy_fixture_workspace("runtime_bridges_defs_instances_lanes_unbound", "local_cmake");
    iv::test_support::write_text(workspace / ".intravenous", "");

    iv::Timeline timeline;
    iv::RuntimeIvModuleDefinitions definitions(workspace, iv::test::repo_root(), {}, {});
    iv::RuntimeIvModuleInstances instances;
    iv::RuntimeGraphInputLanes graph_input_lanes;

    iv::bind_graph_input_lanes_timeline_bridge(timeline);
    (void)definitions.initialize();

    auto const lanes = graph_input_lanes.query_lanes(iv::LaneQueryFilter{.kind = "graphInputs"});
    EXPECT_EQ(lanes.total_lane_count, 0u);

    iv::unbind_graph_input_lanes_timeline_bridge(timeline);
    definitions.request_shutdown();
}

TEST(RuntimeBridges, DefinitionsInstancesAndGraphInputLanesForwardWhenBound)
{
    auto const workspace =
        copy_fixture_workspace("runtime_bridges_defs_instances_lanes_bound", "local_cmake");
    iv::test_support::write_text(workspace / ".intravenous", "");

    iv::Timeline timeline;
    iv::RuntimeIvModuleDefinitions definitions(workspace, iv::test::repo_root(), {}, {});
    iv::RuntimeIvModuleInstances instances;
    iv::RuntimeGraphInputLanes graph_input_lanes;

    iv::bind_graph_input_lanes_timeline_bridge(timeline);
    iv::bind_iv_module_definitions_iv_module_instances_bridge(instances);
    iv::bind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);

    auto const definition = definitions.initialize();
    EXPECT_FALSE(definition.introspection.logical_nodes.empty());

    auto const lanes = graph_input_lanes.query_lanes(iv::LaneQueryFilter{.kind = "graphInputs"});
    EXPECT_GT(lanes.total_lane_count, 0u);

    iv::unbind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
    iv::unbind_iv_module_definitions_iv_module_instances_bridge(instances);
    iv::unbind_graph_input_lanes_timeline_bridge(timeline);
    definitions.request_shutdown();
}
