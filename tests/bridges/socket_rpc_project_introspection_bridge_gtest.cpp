#include "../module_test_utils.h"

#include "runtime/graph_input_lanes.h"
#include "runtime/iv_module_definitions.h"
#include "runtime/iv_module_definitions_project_introspection_bridge.h"
#include "runtime/project_introspection.h"
#include "runtime/socket_rpc_project_introspection_bridge.h"
#include "runtime/socket_rpc_server.h"
#include "runtime/startup_config.h"

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

struct SeededProjectIntrospectionOwner {
    RuntimeIvModuleDefinitions definitions;
    RuntimeGraphInputLanes graph_input_lanes;
    RuntimeProjectIntrospection introspection;
    StartupConfig startup_config;

    SeededProjectIntrospectionOwner(
        std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots)
        : startup_config(
              std::move(workspace_root),
              std::move(discovery_start),
              std::move(extra_search_roots))
    {
        bind_iv_module_definitions_project_introspection_bridge(introspection);
    }

    ~SeededProjectIntrospectionOwner()
    {
        unbind_iv_module_definitions_project_introspection_bridge(introspection);
    }

    void initialize()
    {
        auto const startup = startup_config.initialize();
        auto const module_root =
            std::filesystem::weakly_canonical(startup.workspace_root);
        definitions.seed_loaded_definition(
            iv::test::load_runtime_iv_module_definition(startup, module_root));
        (void)introspection.initialize();
    }
};
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
    SeededProjectIntrospectionOwner owner(workspace, iv::test::repo_root(), {});
    owner.initialize();
    bind_socket_rpc_project_introspection_bridge(
        owner.introspection,
        owner.graph_input_lanes);

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

    unbind_socket_rpc_project_introspection_bridge(
        owner.introspection,
        owner.graph_input_lanes);

    EXPECT_EQ(response["id"], 2);
    ASSERT_TRUE(response["result"].contains("nodes"));
    EXPECT_FALSE(response["result"]["nodes"].empty());
}
