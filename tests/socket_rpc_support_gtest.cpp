#include "runtime/server_options.h"
#include "runtime/socket_rpc_requests.h"
#include "runtime/socket_rpc_response_builders.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <stdexcept>
#include <string_view>

namespace {
    using Json = nlohmann::ordered_json;

    Json parse_json_line(std::string_view line)
    {
        return Json::parse(line);
    }
}

TEST(ServerOptions, ParsesWorkspaceRootAndRpcFd)
{
    char arg0[] = "intravenous";
    char arg1[] = "--server";
    char arg2[] = "--workspace-root";
    char arg3[] = "/tmp/workspace";
    char arg4[] = "--rpc-fd";
    char arg5[] = "7";
    std::array<char*, 6> argv { arg0, arg1, arg2, arg3, arg4, arg5 };

    auto const options = iv::ServerOptions::parse(
        static_cast<int>(argv.size()),
        argv.data());

    EXPECT_EQ(options.workspace_root, "/tmp/workspace");
    EXPECT_EQ(options.rpc_fd, 7);
}

TEST(ServerOptions, RejectsMissingWorkspaceRoot)
{
    char arg0[] = "intravenous";
    char arg1[] = "--server";
    char arg2[] = "--rpc-fd";
    char arg3[] = "7";
    std::array<char*, 4> argv { arg0, arg1, arg2, arg3 };

    EXPECT_THROW(
        (void)iv::ServerOptions::parse(
            static_cast<int>(argv.size()),
            argv.data()),
        std::runtime_error);
}

TEST(ServerOptions, RejectsInvalidRpcFd)
{
    char arg0[] = "intravenous";
    char arg1[] = "--server";
    char arg2[] = "--workspace-root";
    char arg3[] = "/tmp/workspace";
    char arg4[] = "--rpc-fd";
    char arg5[] = "nope";
    std::array<char*, 6> argv { arg0, arg1, arg2, arg3, arg4, arg5 };

    EXPECT_THROW(
        (void)iv::ServerOptions::parse(
            static_cast<int>(argv.size()),
            argv.data()),
        std::runtime_error);
}

TEST(SocketRpcRequestParser, ParsesGraphQueryBySpansRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":21,"method":"graph.queryBySpans","params":{"filePath":"/tmp/module.cpp","ranges":[{"start":{"line":7,"column":2},"end":{"line":9,"column":5}}],"match":"union"}})");

    EXPECT_EQ(parsed.request_id, 21);
    auto const* request = std::get_if<iv::GraphQueryBySpansRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->file_path, "/tmp/module.cpp");
    ASSERT_EQ(request->ranges.size(), 1u);
    EXPECT_EQ(request->ranges[0].start.line, 7u);
    EXPECT_EQ(request->ranges[0].end.column, 5u);
    EXPECT_EQ(request->match_mode, iv::SourceRangeMatchMode::union_);
}

TEST(SocketRpcRequestParser, ParsesLaneViewUpdateRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":22,"method":"timeline.updateLaneView","params":{"viewId":"view-a","filter":{"kind":"graphInputs"},"startIndex":4,"visibleLaneCount":8}})");

    EXPECT_EQ(parsed.request_id, 22);
    auto const* request = std::get_if<iv::UpdateLaneViewRpcRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->request.view_id, "view-a");
    EXPECT_EQ(request->request.query.filter.kind, "graphInputs");
    EXPECT_EQ(request->request.start_index, 4u);
    EXPECT_EQ(request->request.visible_lane_count, 8u);
}

TEST(SocketRpcRequestParser, ParsesSetSampleInputValueRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":23,"method":"graph.setSampleInputValue","params":{"nodeId":"node-1","memberOrdinal":2,"inputOrdinal":5,"value":0.75}})");

    EXPECT_EQ(parsed.request_id, 23);
    auto const* request = std::get_if<iv::SetSampleInputValueRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->node_id, "node-1");
    ASSERT_TRUE(request->member_ordinal.has_value());
    EXPECT_EQ(*request->member_ordinal, 2u);
    EXPECT_EQ(request->input_ordinal, 5u);
    EXPECT_FLOAT_EQ(request->value, 0.75f);
}

TEST(SocketRpcRequestParser, ParsesCreateIvModuleInstanceRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":26,"method":"ivModuleInstances.create","params":{"moduleRoot":"/tmp/mod"}})");

    EXPECT_EQ(parsed.request_id, 26);
    auto const *request = std::get_if<iv::CreateIvModuleInstanceRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->module_root, "/tmp/mod");
}

TEST(SocketRpcRequestParser, ParsesDeleteIvModuleInstanceRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":27,"method":"ivModuleInstances.delete","params":{"instanceId":"instance:1"}})");

    EXPECT_EQ(parsed.request_id, 27);
    auto const *request = std::get_if<iv::DeleteIvModuleInstanceRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->instance_id, "instance:1");
}

TEST(SocketRpcRequestParser, RejectsMissingParamsObject)
{
    EXPECT_THROW(
        (void)iv::parse_socket_rpc_request(
            R"({"jsonrpc":"2.0","id":24,"method":"server.shutdown"})"),
        std::runtime_error);
}

TEST(SocketRpcRequestParser, RejectsUnsupportedMethod)
{
    EXPECT_THROW(
        (void)iv::parse_socket_rpc_request(
            R"({"jsonrpc":"2.0","id":25,"method":"server.nope","params":{}})"),
        std::runtime_error);
}

TEST(SocketRpcAckResponseBuilder, BuildsOkByDefault)
{
    iv::SocketRpcAckResponseBuilder builder;

    auto const response = parse_json_line(builder.build(11));

    EXPECT_EQ(response["jsonrpc"], "2.0");
    EXPECT_EQ(response["id"], 11);
    EXPECT_EQ(response["result"]["ok"], true);
}

TEST(SocketRpcAckResponseBuilder, BuildsErrorAfterFailure)
{
    iv::SocketRpcAckResponseBuilder builder;
    builder.fail(-32055, "bad news");

    auto const response = parse_json_line(builder.build(12));

    EXPECT_EQ(response["jsonrpc"], "2.0");
    EXPECT_EQ(response["id"], 12);
    EXPECT_EQ(response["error"]["code"], -32055);
    EXPECT_EQ(response["error"]["message"], "bad news");
}

TEST(SocketRpcGraphQueryResultBuilder, RequiresExplicitResult)
{
    iv::SocketRpcGraphQueryResultBuilder builder;

    EXPECT_THROW((void)builder.build(13), std::runtime_error);
}

TEST(SocketRpcGraphQueryResultBuilder, SerializesNodes)
{
    iv::SocketRpcGraphQueryResultBuilder builder;
    iv::ProjectQueryResult result;
    result.nodes.push_back(iv::LogicalNodeInfo {
        .id = "node-1",
        .kind = "Oscillator",
        .source_identity = "src-1",
        .type_identity = "Oscillator",
        .source_spans = {
            iv::LiveSourceSpan {
                .file_path = "/tmp/module.cpp",
                .range = {
                    .start = { .line = 3, .column = 4 },
                    .end = { .line = 3, .column = 9 },
                },
            },
        },
        .sample_inputs = {
            iv::LogicalPortInfo {
                .name = "frequency",
                .type = "sample",
                .ordinal = 1,
            },
        },
        .member_count = 0,
    });
    builder.succeed(std::move(result));

    auto const response = parse_json_line(builder.build(14));

    EXPECT_EQ(response["id"], 14);
    ASSERT_EQ(response["result"]["nodes"].size(), 1u);
    auto const& node = response["result"]["nodes"][0];
    EXPECT_EQ(node["id"], "node-1");
    EXPECT_EQ(node["kind"], "Oscillator");
    EXPECT_EQ(node["sourceSpans"][0]["filePath"], "/tmp/module.cpp");
    EXPECT_EQ(node["sourceSpans"][0]["range"]["start"]["line"], 3);
    EXPECT_EQ(node["sampleInputs"][0]["name"], "frequency");
}

TEST(SocketRpcLaneViewResultBuilder, SerializesLaneViewPayload)
{
    iv::SocketRpcLaneViewResultBuilder builder;
    iv::LaneViewResult result {
        .view_id = "view-1",
        .lanes = iv::LaneQueryResult {
            .start_index = 2,
            .visible_lane_count = 3,
            .total_lane_count = 5,
            .lanes = {
                iv::LaneInfo {
                    .lane_id = 42,
                    .domain = iv::LaneDomain::realtime,
                    .graph_input_port = iv::GraphInputPortDescriptor {
                        .logical_node_id = "node-1",
                        .concrete_member_ordinal = 1u,
                        .port_kind = iv::PortKind::sample,
                        .port_ordinal = 7,
                        .port_name = "frequency",
                        .port_type = "sample",
                    },
                },
            },
            .connections = {
                iv::LaneConnectionInfo {
                    .source_lane_id = 42,
                    .target_lane_id = 99,
                    .port_kind = iv::PortKind::sample,
                    .port_ordinal = 7,
                },
            },
        },
    };
    builder.succeed(std::move(result));

    auto const response = parse_json_line(builder.build(15));

    EXPECT_EQ(response["id"], 15);
    EXPECT_EQ(response["result"]["viewId"], "view-1");
    EXPECT_EQ(response["result"]["startIndex"], 2);
    EXPECT_EQ(response["result"]["visibleLaneCount"], 3);
    EXPECT_EQ(response["result"]["totalLaneCount"], 5);
    EXPECT_EQ(response["result"]["lanes"][0]["laneId"], 42);
    EXPECT_EQ(response["result"]["lanes"][0]["domain"], "realtime");
    EXPECT_EQ(response["result"]["lanes"][0]["graphInputPort"]["memberOrdinal"], 1);
    EXPECT_EQ(response["result"]["connections"][0]["targetLaneId"], 99);
}

TEST(SocketRpcCreateIvModuleInstanceResultBuilder, SerializesCreatedInstanceId)
{
    iv::SocketRpcCreateIvModuleInstanceResultBuilder builder;
    builder.succeed("instance:7");

    auto const response = parse_json_line(builder.build(17));

    EXPECT_EQ(response["id"], 17);
    EXPECT_EQ(response["result"]["instanceId"], "instance:7");
}
