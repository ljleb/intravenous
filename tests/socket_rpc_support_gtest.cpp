#include <intravenous/runtime/server_options.h>
#include <intravenous/runtime/socket_rpc_requests.h>
#include <intravenous/runtime/socket_rpc_response_builders.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <limits>
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

TEST(SocketRpcRequestParser, ParsesCreateNodeInstanceRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":26,"method":"nodeInstances.create","params":{"instanceId":"instance:bass","definitionId":"iv.test.mod","displayName":"Bass","configurationSource":"{.gain = 0.5}"}})");

    EXPECT_EQ(parsed.request_id, 26);
    auto const *request = std::get_if<iv::CreateNodeInstanceRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->instance_id, "instance:bass");
    EXPECT_EQ(request->definition_id, "iv.test.mod");
    ASSERT_TRUE(request->display_name.has_value());
    EXPECT_EQ(*request->display_name, "Bass");
    EXPECT_EQ(request->configuration_source, "{.gain = 0.5}");
}

TEST(SocketRpcRequestParser, ParsesGetNodeInstancesRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":40,"method":"nodeInstances.get","params":{"sourceFilePath":"/tmp/module.cpp"}})");

    auto const* request = std::get_if<iv::GetNodeInstancesRequest>(
        &parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->source_file_path, "/tmp/module.cpp");
}

TEST(SocketRpcRequestParser, ParsesUpdateNodeInstancesRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":27,"method":"nodeInstances.update","params":{"updates":[{"instanceId":"instance:1","displayName":"Lead","configurationSource":"880, stereo"}]}})");

    EXPECT_EQ(parsed.request_id, 27);
    auto const *request = std::get_if<iv::UpdateNodeInstancesRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    ASSERT_EQ(request->updates.size(), 1u);
    EXPECT_EQ(request->updates.front().instance_id, "instance:1");
    ASSERT_TRUE(request->updates.front().display_name.has_value());
    EXPECT_EQ(*request->updates.front().display_name, "Lead");
    ASSERT_TRUE(request->updates.front().configuration_source.has_value());
    EXPECT_EQ(*request->updates.front().configuration_source, "880, stereo");
}

TEST(SocketRpcRequestParser, DefaultsMissingConfigurationSourceToEmpty)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":35,"method":"nodeInstances.create","params":{"definitionId":"iv.test.mod"}})");

    auto const* request = std::get_if<iv::CreateNodeInstanceRequest>(
        &parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_TRUE(request->configuration_source.empty());
}

TEST(SocketRpcRequestParser, ParsesDeleteNodeInstanceRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":28,"method":"nodeInstances.delete","params":{"instanceId":"instance:1"}})");

    EXPECT_EQ(parsed.request_id, 28);
    auto const *request = std::get_if<iv::DeleteNodeInstanceRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->instance_id, "instance:1");
}

TEST(SocketRpcRequestParser, ParsesStructuredSampleConnectionMutation)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":36,"method":"graphConnections.upsert","params":{"connection":{"kind":"sample","connectionId":"sample:main","sourceType":"stereo","outputs":[{"instanceId":"source","path":[{"kind":"virtualNode","sourceIdentity":"processor","typeIdentity":"filter"},{"kind":"virtualMember","index":1},{"kind":"tiledChild","name":"right"}],"port":{"name":"audio","index":2,"channel":1}}],"targetType":"mono","inputs":[{"instanceId":"target","path":[],"port":{"name":"audio"}}]}}})");

    EXPECT_EQ(parsed.request_id, 36);
    auto const* request = std::get_if<iv::UpsertGraphConnectionRequest>(
        &parsed.payload);
    ASSERT_NE(request, nullptr);
    auto const* connection = std::get_if<iv::ProjectSampleConnection>(
        &request->connection);
    ASSERT_NE(connection, nullptr);
    EXPECT_EQ(connection->connection_id, "sample:main");
    EXPECT_EQ(connection->source_type, iv::ChannelTypeId::stereo);
    EXPECT_EQ(connection->target_type, iv::ChannelTypeId::mono);
    ASSERT_EQ(connection->outputs.size(), 1u);
    ASSERT_EQ(connection->outputs[0].path.size(), 3u);
    auto const* virtual_node = std::get_if<iv::ProjectVirtualNodeSelector>(
        &connection->outputs[0].path[0]);
    ASSERT_NE(virtual_node, nullptr);
    EXPECT_EQ(virtual_node->source_identity, "processor");
    EXPECT_EQ(virtual_node->type_identity, "filter");
    EXPECT_EQ(connection->outputs[0].port.channel, 1u);
}

TEST(SocketRpcRequestParser, ParsesGraphConnectionDeleteMutation)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":37,"method":"graphConnections.delete","params":{"connectionId":"sample:main"}})");

    auto const* request = std::get_if<iv::DeleteGraphConnectionRequest>(
        &parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->connection_id, "sample:main");
}

TEST(SocketRpcRequestParser, RejectsUnknownStructuredConnectionSelectorsAndTypes)
{
    EXPECT_THROW(
        (void)iv::parse_socket_rpc_request(
            R"({"jsonrpc":"2.0","id":38,"method":"graphConnections.upsert","params":{"connection":{"kind":"sample","connectionId":"bad:type","sourceType":"surround","outputs":[{"instanceId":"source","path":[],"port":{"name":"audio"}}],"targetType":"mono","inputs":[{"instanceId":"target","path":[],"port":{"name":"audio"}}]}}})"),
        std::runtime_error);
    EXPECT_THROW(
        (void)iv::parse_socket_rpc_request(
            R"({"jsonrpc":"2.0","id":39,"method":"graphConnections.upsert","params":{"connection":{"kind":"event","connectionId":"bad:path","sourceType":"midi","outputs":[{"instanceId":"source","path":[{"kind":"subgraph","index":0}],"port":{"name":"events"}}],"targetType":"trigger","inputs":[{"instanceId":"target","path":[],"port":{"name":"events"}}]}}})"),
        std::runtime_error);
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
        .continuous_inputs = {
            iv::VirtualContinuousPortInfo {
                .name = "frequency",
                .index = 1,
                .value_types = {
                    iv::SampleContinuousValueExtension::IntrospectionMetadata{
                        .default_value = iv::Sample{0.5f},
                        .min = iv::Sample{
                            -std::numeric_limits<iv::Sample::storage>::infinity()},
                        .max = iv::Sample{
                            std::numeric_limits<iv::Sample::storage>::infinity()},
                    },
                    iv::FractionalGlobalIndexContinuousValueExtension::
                        IntrospectionMetadata{
                            .default_value = iv::FractionalGlobalIndex::from_parts(
                                iv::GlobalIndex::from_bits(
                                    std::uint64_t{9007199254740993ULL}),
                                std::uint64_t{18446744073709551615ULL}),
                        },
                    iv::FFTBlockContinuousValueExtension::IntrospectionMetadata{},
                },
                .channel_type = iv::ChannelTypeId::mono,
                .binding = iv::VirtualPortBinding::interface,
            },
        },
        .event_inputs = {
            iv::VirtualEventPortInfo{
                .name = "trigger",
                .index = 2,
                .type = iv::EventTypeId::trigger,
                .binding = iv::VirtualPortBinding::virtual_input,
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
    auto const& continuous = node["continuousInputs"][0];
    EXPECT_EQ(continuous["name"], "frequency");
    EXPECT_EQ(continuous["index"], 1);
    EXPECT_EQ(continuous["binding"], "interface");
    EXPECT_EQ(continuous["channelType"], "mono");
    ASSERT_EQ(continuous["valueTypes"].size(), 3u);
    EXPECT_EQ(continuous["valueTypes"][0]["type"], "Sample");
    EXPECT_EQ(continuous["valueTypes"][0]["defaultValue"], 0.5f);
    EXPECT_EQ(
        continuous["valueTypes"][0]["minValue"], "negativeInfinity");
    EXPECT_EQ(
        continuous["valueTypes"][0]["maxValue"], "positiveInfinity");
    EXPECT_EQ(
        continuous["valueTypes"][1]["defaultValue"]["wholeBits"],
        "9007199254740993");
    EXPECT_EQ(
        continuous["valueTypes"][1]["defaultValue"]["fractionBits"],
        "18446744073709551615");
    EXPECT_EQ(continuous["valueTypes"][2]["type"], "FFTBlock");
    EXPECT_EQ(continuous["valueTypes"][2]["rank"], 1);
    EXPECT_FALSE(continuous["valueTypes"][2].contains("defaultValue"));
    EXPECT_FALSE(continuous.contains("currentValue"));
    EXPECT_FALSE(continuous.contains("hasConcreteOverride"));
    EXPECT_FALSE(continuous.contains("stateValue"));
    EXPECT_EQ(node["eventInputs"][0]["eventType"], "trigger");
    EXPECT_EQ(node["eventInputs"][0]["binding"], "virtualInput");
    EXPECT_EQ(node["members"][0]["index"], 4);
}









TEST(SocketRpcCreateNodeInstanceResultBuilder, SerializesCreatedInstanceId)
{
    iv::SocketRpcCreateNodeInstanceResultBuilder builder;
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
