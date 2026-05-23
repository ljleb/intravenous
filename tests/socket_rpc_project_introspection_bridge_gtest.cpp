#include "module_test_utils.h"
#include "runtime/graph_input_lanes.h"
#include "runtime/graph_input_lanes_lane_views_bridge.h"
#include "runtime/graph_input_lanes_timeline_bridge.h"
#include "runtime/iv_module_definitions.h"
#include "runtime/iv_module_definitions_iv_module_instances_bridge.h"
#include "runtime/iv_module_definitions_project_introspection_bridge.h"
#include "runtime/iv_module_instances.h"
#include "runtime/iv_module_instances_graph_input_lanes_bridge.h"
#include "runtime/lane_views.h"
#include "runtime/lane_views_timeline_bridge.h"
#include "runtime/project_introspection.h"
#include "runtime/project_introspection_graph_input_lanes_bridge.h"
#include "runtime/socket_rpc_lane_views_bridge.h"
#include "runtime/socket_rpc_project_introspection_bridge.h"
#include "runtime/socket_rpc_server.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::ordered_json;
using namespace iv;

std::filesystem::path make_project_workspace()
{
    auto const workspace = iv::test::fresh_test_workspace("socket_rpc_project_introspection_bridge");
    iv::test::copy_directory(iv::test::test_modules_root() / "local_cmake", workspace);
    std::ofstream marker(workspace / ".intravenous", std::ios::binary | std::ios::trunc);
    EXPECT_TRUE(static_cast<bool>(marker));
    return workspace;
}

Json parse_json_line(std::string_view line)
{
    return Json::parse(line);
}
} // namespace

TEST(SocketRpcProjectIntrospectionBridge, UnboundQueryEventLeavesBuilderUnbuilt)
{
    auto const workspace = make_project_workspace();
    SocketRpcGraphQueryResultBuilder builder;

    IV_INVOKE_LINKER_EVENT(
        iv_socket_rpc_graph_query_by_spans_event,
        GraphQueryBySpansRequest{
            .file_path = workspace / "module.cpp",
            .ranges = {
                iv::SourceRange{
                    .start = {.line = 7, .column = 1},
                    .end = {.line = 15, .column = 1},
                },
            },
            .match_mode = iv::SourceRangeMatchMode::intersection,
        },
        builder);

    EXPECT_THROW((void)builder.build(1), std::runtime_error);
}

TEST(SocketRpcProjectIntrospectionBridge, BoundQueryEventPopulatesBuilderFromOwners)
{
    auto const workspace = make_project_workspace();
    Timeline timeline;
    RuntimeIvModuleDefinitions iv_module_definitions(
        workspace,
        iv::test::repo_root(),
        std::vector<std::filesystem::path>{},
        {});
    RuntimeIvModuleInstances iv_module_instances;
    RuntimeGraphInputLanes graph_input_lanes;
    RuntimeLaneViews lane_views;
    RuntimeProjectIntrospection introspection;

    bind_graph_input_lanes_timeline_bridge(timeline);
    bind_graph_input_lanes_lane_views_bridge(graph_input_lanes);
    bind_iv_module_definitions_iv_module_instances_bridge(iv_module_instances);
    bind_iv_module_definitions_project_introspection_bridge(introspection);
    bind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
    bind_project_introspection_graph_input_lanes_bridge(graph_input_lanes);
    bind_lane_views_timeline_bridge(lane_views);
    bind_socket_rpc_lane_views_bridge(lane_views);
    (void)iv_module_definitions.initialize();
    introspection.initialize();
    bind_socket_rpc_project_introspection_bridge(introspection, graph_input_lanes);

    SocketRpcGraphQueryResultBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv_socket_rpc_graph_query_by_spans_event,
        GraphQueryBySpansRequest{
            .file_path = workspace / "module.cpp",
            .ranges = {
                iv::SourceRange{
                    .start = {.line = 7, .column = 1},
                    .end = {.line = 15, .column = 1},
                },
            },
            .match_mode = iv::SourceRangeMatchMode::intersection,
        },
        builder);

    auto const response = parse_json_line(builder.build(2));

    unbind_socket_rpc_project_introspection_bridge(introspection, graph_input_lanes);
    unbind_socket_rpc_lane_views_bridge(lane_views);
    unbind_lane_views_timeline_bridge(lane_views);
    unbind_project_introspection_graph_input_lanes_bridge(graph_input_lanes);
    unbind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
    unbind_iv_module_definitions_project_introspection_bridge(introspection);
    unbind_iv_module_definitions_iv_module_instances_bridge(iv_module_instances);
    unbind_graph_input_lanes_lane_views_bridge(graph_input_lanes);
    unbind_graph_input_lanes_timeline_bridge(timeline);
    iv_module_definitions.request_shutdown();

    EXPECT_EQ(response["id"], 2);
    ASSERT_TRUE(response["result"].contains("nodes"));
    EXPECT_FALSE(response["result"]["nodes"].empty());
}
