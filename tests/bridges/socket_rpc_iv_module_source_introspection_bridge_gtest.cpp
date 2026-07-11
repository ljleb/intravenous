#include "../module_test_utils.h"

#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_instances_bridge.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_instances_iv_module_definitions_bridge.h>
#include <intravenous/runtime/iv_module_instances_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>
#include <intravenous/runtime/socket_rpc_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/startup_config.h>

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
bool g_answer_authored_state_snapshot = false;

IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleSourceIntrospectionLiveInputSnapshotsRequestedEvent,
    iv_runtime_iv_module_source_introspection_live_input_snapshots_requested_event,
    +[](std::vector<IvModuleSourceIntrospectionLiveInputSnapshotRequest> const &requests,
        IvModuleSourceIntrospectionLiveInputSnapshotsBuilder &builder) {
        if (!g_answer_live_input_snapshots) {
            return;
        }

        std::vector<IvModuleSourceIntrospectionLiveInputSnapshot> snapshots;
        snapshots.reserve(requests.size());
        for (auto const &request : requests) {
            snapshots.push_back(IvModuleSourceIntrospectionLiveInputSnapshot{
                .logical_node_id = request.logical_node_id,
                .member_ordinal = request.member_ordinal,
                .input_ordinal = request.input_ordinal,
                .current_value = request.fallback,
                .has_concrete_override = false,
            });
        }
        builder.succeed(std::move(snapshots));
    });

IV_SUBSCRIBE_LINKER_EVENT(
    IvModuleSourceIntrospectionAuthoredStateSnapshotRequestedEvent,
    iv_runtime_iv_module_source_introspection_authored_state_snapshot_requested_event,
    +[](IvModuleSourceIntrospectionAuthoredStateSnapshotBuilder &builder) {
        if (!g_answer_authored_state_snapshot) {
            return;
        }
        builder.succeed(IvModuleSourceIntrospectionAuthoredStateSnapshot{});
    });

std::filesystem::path make_project_workspace()
{
    return iv::test::read_only_module_fixture_workspace("local_cmake");
}

Json parse_json_line(std::string_view line)
{
    return Json::parse(line);
}

struct SeededIvModuleSourceIntrospectionOwner {
    IvModuleInstances instances;
    IvModuleDefinitions definitions;
    GraphInputLanes graph_input_lanes;
    IvModuleSourceIntrospection introspection;
    StartupConfig startup_config;

    SeededIvModuleSourceIntrospectionOwner(
        std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots)
        : startup_config(
              std::move(workspace_root),
              std::move(discovery_start),
              std::move(extra_search_roots))
    {
    }

    ~SeededIvModuleSourceIntrospectionOwner() = default;

    void initialize()
    {
        bind_iv_module_instances_iv_module_definitions_bridge(definitions);
        bind_iv_module_definitions_iv_module_instances_bridge(instances);
        bind_iv_module_definitions_iv_module_source_introspection_bridge(introspection);
        bind_iv_module_instances_iv_module_source_introspection_bridge(introspection);
        auto const startup = startup_config.initialize();
        auto const module_root =
            std::filesystem::weakly_canonical(startup.workspace_root);
        auto definition = iv::test::load_runtime_iv_module_definition(startup, module_root);
        (void)instances.create_instance(
            definition.module_id,
            module_root,
            "instance:1");
        definitions.seed_loaded_definition(std::move(definition));
    }

    void shutdown()
    {
        unbind_iv_module_instances_iv_module_source_introspection_bridge(introspection);
        unbind_iv_module_definitions_iv_module_source_introspection_bridge(introspection);
        unbind_iv_module_definitions_iv_module_instances_bridge(instances);
        unbind_iv_module_instances_iv_module_definitions_bridge(definitions);
    }
};
} // namespace

TEST(SocketRpcIvModuleSourceIntrospectionBridge, UnboundQueryEventLeavesBuilderUnbuilt)
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
            .instance_id = "instance:1",
        },
        builder);

    EXPECT_THROW((void)builder.build(1), std::runtime_error);
}

TEST(SocketRpcIvModuleSourceIntrospectionBridge, BoundQueryEventPopulatesBuilderFromOwners)
{
    auto const workspace = make_project_workspace();
    g_answer_live_input_snapshots = true;
    g_answer_authored_state_snapshot = true;
    SeededIvModuleSourceIntrospectionOwner owner(workspace, iv::test::repo_root(), {});
    owner.initialize();
    bind_socket_rpc_iv_module_source_introspection_bridge(
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
            .instance_id = "instance:1",
        },
        builder);

    auto const response = parse_json_line(builder.build(2));

    unbind_socket_rpc_iv_module_source_introspection_bridge(
        owner.introspection,
        owner.graph_input_lanes);
    owner.shutdown();
    g_answer_live_input_snapshots = false;
    g_answer_authored_state_snapshot = false;

    EXPECT_EQ(response["id"], 2);
    ASSERT_TRUE(response["result"].contains("nodes"));
    EXPECT_FALSE(response["result"]["nodes"].empty());
    EXPECT_EQ(response["result"]["nodes"][0]["instanceId"], "instance:1");
}
