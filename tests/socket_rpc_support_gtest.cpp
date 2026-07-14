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

    iv::InternedString intern(std::string_view value)
    {
        return iv::InternedString::from_view(value);
    }

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
    EXPECT_EQ(request->request.view_id.str(), "view-a");
    EXPECT_EQ(request->request.query.filter.source, "dsp_graph.graph_input");
    EXPECT_EQ(request->request.start_index, 4u);
    EXPECT_EQ(request->request.visible_lane_count, 8u);
}

TEST(SocketRpcRequestParser, ParsesLaneViewCompiledEventDisplayWindow)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":23,"method":"timeline.updateLaneView","params":{"viewId":"view-a","filter":{"kind":"graphInputs"},"firstSampleIndex":48000,"lastSampleIndex":144000,"displaySampleCount":96000}})");
    auto const* request = std::get_if<iv::UpdateLaneViewRpcRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->request.first_sample_index, 48000u);
    EXPECT_EQ(request->request.last_sample_index, 144000u);
    EXPECT_EQ(request->request.display_sample_count, 96000u);
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

TEST(SocketRpcRequestParser, ParsesSetSampleInputStateRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":24,"method":"graph.setSampleInputState","params":{"nodeId":"node-1","memberOrdinal":2,"inputOrdinal":5,"state":"timelineLane"}})");

    EXPECT_EQ(parsed.request_id, 24);
    auto const* request = std::get_if<iv::SetSampleInputStateRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->node_id, "node-1");
    ASSERT_TRUE(request->member_ordinal.has_value());
    EXPECT_EQ(*request->member_ordinal, 2u);
    EXPECT_EQ(request->input_ordinal, 5u);
    EXPECT_EQ(request->state, "timelineLane");
}

TEST(SocketRpcRequestParser, ParsesDefaultSampleInputStateRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":25,"method":"graph.setSampleInputState","params":{"nodeId":"node-1","memberOrdinal":2,"inputOrdinal":5,"state":"default"}})");

    EXPECT_EQ(parsed.request_id, 25);
    auto const* request = std::get_if<iv::SetSampleInputStateRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->state, "default");
}

TEST(SocketRpcRequestParser, ParsesSetEventInputStateRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":26,"method":"graph.setEventInputState","params":{"nodeId":"node-1","memberOrdinal":2,"inputOrdinal":5,"state":"logicalFollow"}})");

    EXPECT_EQ(parsed.request_id, 26);
    auto const* request = std::get_if<iv::SetEventInputStateRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->node_id, "node-1");
    ASSERT_TRUE(request->member_ordinal.has_value());
    EXPECT_EQ(*request->member_ordinal, 2u);
    EXPECT_EQ(request->input_ordinal, 5u);
    EXPECT_EQ(request->state, "logicalFollow");
}

TEST(SocketRpcRequestParser, ParsesSetSampleOutputStateRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":27,"method":"graph.setSampleOutputState","params":{"nodeId":"node-1","memberOrdinal":2,"outputOrdinal":5,"state":"logical"}})");

    EXPECT_EQ(parsed.request_id, 27);
    auto const* request = std::get_if<iv::SetSampleOutputStateRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->node_id, "node-1");
    ASSERT_TRUE(request->member_ordinal.has_value());
    EXPECT_EQ(*request->member_ordinal, 2u);
    EXPECT_EQ(request->output_ordinal, 5u);
    EXPECT_EQ(request->state, "logical");
}

TEST(SocketRpcRequestParser, ParsesSetEventOutputStateRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":28,"method":"graph.setEventOutputState","params":{"nodeId":"node-1","outputOrdinal":3,"state":"timelineLane"}})");

    EXPECT_EQ(parsed.request_id, 28);
    auto const* request = std::get_if<iv::SetEventOutputStateRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->node_id, "node-1");
    EXPECT_FALSE(request->member_ordinal.has_value());
    EXPECT_EQ(request->output_ordinal, 3u);
    EXPECT_EQ(request->state, "timelineLane");
}

TEST(SocketRpcRequestParser, ParsesSetTimelineLaneSampleChannelTypeRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":29,"method":"timeline.setLaneSampleChannelType","params":{"laneId":"lane-42","sampleChannelType":"mono"}})");

    EXPECT_EQ(parsed.request_id, 29);
    auto const* request = std::get_if<iv::SetTimelineLaneSampleChannelTypeRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->lane_id.str(), "lane-42");
    EXPECT_EQ(request->sample_channel_type, iv::ChannelTypeId::mono);
}

TEST(SocketRpcRequestParser, ParsesGetAudioDevicesRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":30,"method":"audioDevices.get","params":{}})");

    EXPECT_EQ(parsed.request_id, 30);
    auto const *request = std::get_if<iv::GetAudioDevicesRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
}

TEST(SocketRpcRequestParser, ParsesSetAudioDevicesRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":31,"method":"audioDevices.set","params":{"outputDeviceId":"default","inputDeviceId":null}})");

    EXPECT_EQ(parsed.request_id, 31);
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

TEST(SocketRpcRequestParser, ParsesPlaybackPauseRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":33,"method":"playback.pause","params":{}})");

    EXPECT_EQ(parsed.request_id, 33);
    auto const *request = std::get_if<iv::PauseRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
}

TEST(SocketRpcRequestParser, ParsesPlaybackResumeRequest)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":34,"method":"playback.resume","params":{"startIndex":96}})");

    EXPECT_EQ(parsed.request_id, 34);
    auto const *request = std::get_if<iv::ResumeRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->start_index, 96u);
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
        R"({"jsonrpc":"2.0","id":27,"method":"ivModuleInstances.update","params":{"updates":[{"instanceId":"instance:1","displayName":"Lead","defaultSilenceTtlSamples":64}]}})");

    EXPECT_EQ(parsed.request_id, 27);
    auto const *request = std::get_if<iv::UpdateIvModuleInstancesRequest>(&parsed.payload);
    ASSERT_NE(request, nullptr);
    ASSERT_EQ(request->updates.size(), 1u);
    EXPECT_EQ(request->updates.front().instance_id, "instance:1");
    ASSERT_TRUE(request->updates.front().display_name.has_value());
    EXPECT_EQ(*request->updates.front().display_name, "Lead");
    ASSERT_TRUE(request->updates.front().default_silence_ttl_samples.has_value());
    EXPECT_EQ(*request->updates.front().default_silence_ttl_samples, 64u);
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
        .view_id = intern("view-1"),
        .lanes = iv::LaneQueryResult {
            .start_index = 2,
            .visible_lane_count = 3,
            .total_lane_count = 5,
            .lanes = {
                iv::LaneInfo {
                    .lane_id = intern("42"),
                    .domain = iv::LaneDomain::realtime,
                    .metadata = iv::LaneMetadata{
                        .unit_values = {"graph_input"},
                        .int_values = {
                            {"port_ordinal", 7},
                            {"member_ordinal", 1},
                        },
                    },
                },
            },
            .connections = {
                iv::LaneConnectionInfo {
                    .source_lane_id = intern("lane-42"),
                    .target_lane_id = intern("lane-99"),
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
    EXPECT_EQ(response["result"]["lanes"][0]["laneId"], "42");
    EXPECT_EQ(response["result"]["lanes"][0]["domain"], "realtime");
    EXPECT_TRUE(response["result"]["lanes"][0].contains("metadata"));
    EXPECT_EQ(response["result"]["lanes"][0]["metadata"]["member_ordinal"], 1);
    EXPECT_EQ(response["result"]["connections"][0]["targetLaneId"], "lane-99");
}

TEST(SocketRpcLaneViewResultBuilder, SerializesLaneViewErrorPayload)
{
    iv::SocketRpcLaneViewResultBuilder builder;
    builder.succeed(iv::LaneViewResult{
        .view_id = intern("view-1"),
        .lanes = iv::LaneQueryResult{
            .error_message = "bad filter",
        },
    });

    auto const response = parse_json_line(builder.build(16));

    EXPECT_EQ(response["id"], 16);
    EXPECT_EQ(response["result"]["viewId"], "view-1");
    EXPECT_EQ(response["result"]["error"], "bad filter");
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
