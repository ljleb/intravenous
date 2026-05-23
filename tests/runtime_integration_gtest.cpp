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

TEST(RuntimeIntegration, DefinitionsAndProjectIntrospectionInitializeAndShutdown)
{
    auto const workspace =
        copy_fixture_workspace("runtime_integration_project_introspection", "local_cmake");
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

TEST(RuntimeIntegration, DefinitionsInstancesAndGraphInputLanesInitializeAndShutdown)
{
    auto const workspace =
        copy_fixture_workspace("runtime_integration_graph_input_lanes", "local_cmake");
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

TEST(RuntimeIntegration, QueryBySpansReturnsMatchingLiveNodesWithPorts)
{
    auto const workspace =
        copy_fixture_workspace("runtime_integration_query_by_spans", "local_cmake");
    iv::test_support::write_text(workspace / ".intravenous", "");

    iv::test_support::BoundRuntimeProjectIntrospection app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {
            iv::SourceRange{
                .start = {.line = 7, .column = 1},
                .end = {.line = 15, .column = 1},
            },
        });
    ASSERT_FALSE(result.nodes.empty());
    EXPECT_FALSE(result.nodes.front().id.empty());
}
