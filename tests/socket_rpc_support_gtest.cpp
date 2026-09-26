#include <intravenous/runtime/server_options.h>
#include <intravenous/runtime/socket_rpc_requests.h>
#include <intravenous/runtime/socket_rpc_response_builders.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
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









TEST(SocketRpcRequestParser, ParsesGetAudioDevicesRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":31,"method":"audioDevices.get","params":{}})");

    EXPECT_EQ(parsed.request_id, 31);
    auto const *request = std::get_if<iv::GetAudioDevicesRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
}

TEST(SocketRpcRequestParser, ParsesSetAudioDevicesRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":32,"method":"audioDevices.set","params":{"outputDeviceId":"default","inputDeviceId":null}})");

    EXPECT_EQ(parsed.request_id, 32);
    auto const *request = std::get_if<iv::SetAudioDevicesRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    ASSERT_TRUE(request->output_device_id.has_value());
    EXPECT_EQ(*request->output_device_id, "default");
    EXPECT_FALSE(request->input_device_id.has_value());
}

TEST(SocketRpcRequestParser, ParsesProjectSaveRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":32,"method":"project.save","params":{}})");

    EXPECT_EQ(parsed.request_id, 32);
    auto const *request = std::get_if<iv::SaveProjectRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
}

TEST(SocketRpcRequestParser, ParsesProjectAutosaveRequests)
{
    auto const enable = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":33,"method":"project.enableAutosave","params":{}})");
    EXPECT_EQ(enable.request_id, 33);
    EXPECT_NE(
        std::get_if<iv::EnableProjectAutosaveRequest>(&enable.payload),
        nullptr);

    auto const disable = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":34,"method":"project.disableAutosave","params":{}})");
    EXPECT_EQ(disable.request_id, 34);
    EXPECT_NE(
        std::get_if<iv::DisableProjectAutosaveRequest>(&disable.payload),
        nullptr);
}

TEST(SocketRpcRequestParser, ParsesCreateIvModuleInstanceRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":26,"method":"ivModuleInstances.create","params":{"moduleId":"iv.test.mod","displayName":"Bass"}})");

    EXPECT_EQ(parsed.request_id, 26);
    auto const *request = std::get_if<iv::CreateIvModuleInstanceRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->module_id, "iv.test.mod");
    ASSERT_TRUE(request->display_name.has_value());
    EXPECT_EQ(*request->display_name, "Bass");
}

TEST(SocketRpcRequestParser, ParsesUpdateIvModuleInstancesRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":27,"method":"ivModuleInstances.update","params":{"updates":[{"instanceId":"instance:1","displayName":"Lead"}]}})");

    EXPECT_EQ(parsed.request_id, 27);
    auto const *request = std::get_if<iv::UpdateIvModuleInstancesRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    ASSERT_EQ(request->updates.size(), 1u);
    EXPECT_EQ(request->updates.front().instance_id, "instance:1");
    ASSERT_TRUE(request->updates.front().display_name.has_value());
    EXPECT_EQ(*request->updates.front().display_name, "Lead");
}

TEST(SocketRpcRequestParser, ParsesDeleteIvModuleInstanceRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":28,"method":"ivModuleInstances.delete","params":{"instanceId":"instance:1"}})");

    EXPECT_EQ(parsed.request_id, 28);
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

TEST(SocketRpcRequestParser, PreservesUnsupportedMethodForDispatch)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":25,"method":"server.nope","params":{}})");

    EXPECT_EQ(parsed.request_id, 25);
    auto const *request = std::get_if<iv::UnsupportedSocketRpcRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->method, "server.nope");
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
    result.nodes.push_back(iv::VirtualNodeInfo {
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
            iv::VirtualPortInfo {
                .name = "frequency",
                .type = "sample",
                .index = 1,
            },
        },
        .member_count = 1,
        .members = {
            iv::VirtualNodeMemberInfo {
                .index = 4,
                .backing_node_id = "backing-1",
            },
        },
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
    EXPECT_EQ(node["sampleInputs"][0]["index"], 1);
    EXPECT_EQ(node["members"][0]["index"], 4);
}









TEST(SocketRpcCreateIvModuleInstanceResultBuilder, SerializesCreatedInstanceId)
{
    iv::SocketRpcCreateIvModuleInstanceResultBuilder builder;
    builder.succeed("instance:7");

    auto const response = parse_json_line(builder.build(17));

    EXPECT_EQ(response["id"], 17);
    EXPECT_EQ(response["result"]["instanceId"], "instance:7");
}

TEST(SocketRpcAudioDevicesResultBuilder, SerializesSnapshot)
{
    iv::SocketRpcAudioDevicesResultBuilder builder;
    builder.succeed(iv::AudioDevicesSnapshot{
        .output_devices = {
            iv::AudioDeviceDescriptor{.device_id = "default", .name = "System Default"},
            iv::AudioDeviceDescriptor{.device_id = "out-1", .name = "Output 1"},
        },
        .input_devices = {
            iv::AudioDeviceDescriptor{.device_id = "default", .name = "System Default"},
            iv::AudioDeviceDescriptor{.device_id = "in-1", .name = "Input 1"},
        },
        .selected_output = iv::AudioDeviceSelectionState{
            .device_id = std::string("out-1"),
            .name = std::string("Output 1"),
            .available = true,
        },
        .selected_input = iv::AudioDeviceSelectionState{
            .device_id = std::string("missing"),
            .name = std::nullopt,
            .available = false,
        },
    });

    auto const response = parse_json_line(builder.build(18));

    EXPECT_EQ(response["id"], 18);
    EXPECT_EQ(response["result"]["outputDevices"].size(), 2u);
    EXPECT_EQ(response["result"]["outputDevices"][1]["deviceId"], "out-1");
    EXPECT_EQ(response["result"]["selectedOutput"]["name"], "Output 1");
    EXPECT_EQ(response["result"]["selectedInput"]["deviceId"], "missing");
    EXPECT_EQ(response["result"]["selectedInput"]["available"], false);
}
