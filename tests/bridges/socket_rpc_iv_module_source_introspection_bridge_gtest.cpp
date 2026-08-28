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
#include <intravenous/runtime/iv_module_source_introspection_graph_input_lanes_bridge.h>
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
    iv_module_definitions_iv_module_instances_bridge::scope
        iv_module_definitions_iv_module_instances_scope;
    iv_module_instances_iv_module_definitions_bridge::scope
        iv_module_instances_iv_module_definitions_scope;
    iv_module_definitions_iv_module_source_introspection_bridge::scope
        iv_module_definitions_iv_module_source_introspection_scope;
    iv_module_instances_iv_module_source_introspection_bridge::scope
        iv_module_instances_iv_module_source_introspection_scope;
    iv_module_source_introspection_graph_input_lanes_bridge::scope
        iv_module_source_introspection_graph_input_lanes_scope;

    SeededIvModuleSourceIntrospectionOwner(
        std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots)
        : startup_config(
              std::move(workspace_root),
              std::move(discovery_start),
              std::move(extra_search_roots)),
          iv_module_definitions_iv_module_instances_scope(definitions, instances),
          iv_module_instances_iv_module_definitions_scope(instances, definitions),
          iv_module_definitions_iv_module_source_introspection_scope(
              definitions,
              introspection),
          iv_module_instances_iv_module_source_introspection_scope(
              instances,
              introspection),
          iv_module_source_introspection_graph_input_lanes_scope(
              introspection,
              graph_input_lanes)
    {
    }

    ~SeededIvModuleSourceIntrospectionOwner() = default;

    void initialize()
    {
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
    SeededIvModuleSourceIntrospectionOwner owner(workspace, iv::test::repo_root(), {});
    owner.initialize();
    SocketRpcServer server(workspace, -1);
    auto socket_introspection_scope =
        socket_rpc_iv_module_source_introspection_bridge::bind(server, owner.introspection);

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

    owner.shutdown();

    EXPECT_EQ(response["id"], 2);
    ASSERT_TRUE(response["result"].contains("nodes"));
    EXPECT_FALSE(response["result"]["nodes"].empty());
    EXPECT_EQ(response["result"]["nodes"][0]["instanceId"], "instance:1");
}
