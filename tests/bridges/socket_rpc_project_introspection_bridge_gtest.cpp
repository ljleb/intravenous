#include "../module_test_utils.h"

#include "runtime/graph_input_lanes.h"
#include "runtime/iv_module_definitions.h"
#include "runtime/project_introspection.h"
#include "runtime/project_introspection_events.h"
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
bool g_answer_live_input_snapshots = false;

IV_SUBSCRIBE_LINKER_EVENT(
    ProjectIntrospectionLiveInputSnapshotsRequestedEvent,
    iv_runtime_project_introspection_live_input_snapshots_requested_event,
    +[](std::vector<ProjectIntrospectionLiveInputSnapshotRequest> const &requests,
        ProjectIntrospectionLiveInputSnapshotsBuilder &builder) {
        if (!g_answer_live_input_snapshots) {
            return;
        }

        std::vector<ProjectIntrospectionLiveInputSnapshot> snapshots;
        snapshots.reserve(requests.size());
        for (auto const &request : requests) {
            snapshots.push_back(ProjectIntrospectionLiveInputSnapshot{
                .logical_node_id = request.logical_node_id,
                .member_ordinal = request.member_ordinal,
                .input_ordinal = request.input_ordinal,
                .current_value = request.fallback,
                .has_concrete_override = false,
            });
        }
        builder.succeed(std::move(snapshots));
    });

std::filesystem::path make_project_workspace()
{
    return iv::test::shared_project_fixture_workspace("local_cmake");
}

Json parse_json_line(std::string_view line)
{
    return Json::parse(line);
}

struct SeededProjectIntrospectionOwner {
    GraphInputLanes graph_input_lanes;
    ProjectIntrospection introspection;
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
    }

    ~SeededProjectIntrospectionOwner() = default;

    void initialize()
    {
        auto const startup = startup_config.initialize();
        auto const module_root =
            std::filesystem::weakly_canonical(startup.workspace_root);
        auto loaded =
            iv::test::load_runtime_iv_module_definition(startup, module_root);
        introspection.handle_iv_module_definitions_changed(
            IvModuleDefinitionsChanged{
                .created = {IvModuleDefinition{
                    .definition_id = loaded.definition_id,
                    .module_root = loaded.module_root,
                    .module_id = loaded.module_id,
                    .introspection = loaded.introspection,
                    .dependencies = loaded.dependencies,
                    .canonical_builder = &loaded.canonical_builder,
                }},
            });
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
    g_answer_live_input_snapshots = true;
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
    g_answer_live_input_snapshots = false;

    EXPECT_EQ(response["id"], 2);
    ASSERT_TRUE(response["result"].contains("nodes"));
    EXPECT_FALSE(response["result"]["nodes"].empty());
}
