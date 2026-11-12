#include <intravenous/runtime/server_options.h>
#include <intravenous/runtime/socket_rpc_requests.h>
#include <intravenous/runtime/socket_rpc_response_builders.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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

TEST(ServerOptions, AllowsProjectSessionDiscoveryWithoutInheritedRpcFd)
{
    char arg0[] = "intravenous";
    char arg1[] = "--server";
    char arg2[] = "--workspace-root";
    char arg3[] = "/tmp/workspace";
    std::array<char*, 4> argv { arg0, arg1, arg2, arg3 };

    auto const options = iv::ServerOptions::parse(
        static_cast<int>(argv.size()), argv.data());

    EXPECT_EQ(options.workspace_root, "/tmp/workspace");
    EXPECT_EQ(options.rpc_fd, -1);
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

TEST(SocketRpcRequestParser, ParsesManualControlDurableAndInteractionRequests)
{
    auto const upsert = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":40,"method":"manualControls.upsert","params":{"node":{"instanceId":"instance:synth","path":[{"kind":"virtualNode","sourceIdentity":"voice"}]},"inputName":"gain","value":0.625,"participation":"participateWithConnections","enabled":true}})");
    auto const* durable = std::get_if<iv::UpsertManualControlRequest>(
        &upsert.payload);
    ASSERT_NE(durable, nullptr);
    EXPECT_EQ(durable->control.id.node.instance_id, "instance:synth");
    ASSERT_EQ(durable->control.id.node.path.size(), 1u);
    EXPECT_EQ(durable->control.id.input_name, "gain");
    EXPECT_FLOAT_EQ(static_cast<float>(durable->control.stored_value), 0.625f);
    EXPECT_EQ(
        durable->control.participation,
        iv::ManualControlParticipation::participate_with_connections);
    EXPECT_TRUE(durable->control.enabled);

    auto const hover = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":41,"method":"manualControls.hover","params":{"node":{"instanceId":"instance:synth","path":[]},"inputName":"gain"}})");
    auto const* hovered = std::get_if<iv::SetManualControlHoverRequest>(
        &hover.payload);
    ASSERT_NE(hovered, nullptr);
    EXPECT_TRUE(hovered->client_id.empty());
    ASSERT_TRUE(hovered->id);
    EXPECT_EQ(hovered->id->input_name, "gain");

    auto const begin = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":42,"method":"manualControls.begin","params":{"interactionId":"pointer:3","node":{"instanceId":"instance:synth","path":[]},"inputName":"gain","value":0.75}})");
    auto const* started =
        std::get_if<iv::BeginManualControlInteractionRequest>(&begin.payload);
    ASSERT_NE(started, nullptr);
    EXPECT_TRUE(started->interaction.client_id.empty());
    EXPECT_EQ(started->interaction.interaction_id, "pointer:3");
    EXPECT_EQ(started->id.input_name, "gain");
    EXPECT_FLOAT_EQ(static_cast<float>(started->value), 0.75f);

    auto const update = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":43,"method":"manualControls.update","params":{"interactionId":"pointer:3","value":0.875}})");
    auto const* changed =
        std::get_if<iv::UpdateManualControlInteractionRequest>(&update.payload);
    ASSERT_NE(changed, nullptr);
    EXPECT_EQ(changed->interaction.interaction_id, "pointer:3");
    EXPECT_FLOAT_EQ(static_cast<float>(changed->value), 0.875f);

    auto const end = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":44,"method":"manualControls.end","params":{"interactionId":"pointer:3","value":1.0}})");
    auto const* stopped =
        std::get_if<iv::EndManualControlInteractionRequest>(&end.payload);
    ASSERT_NE(stopped, nullptr);
    EXPECT_TRUE(stopped->interaction.client_id.empty());
    EXPECT_FLOAT_EQ(static_cast<float>(stopped->value), 1.0f);
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

TEST(SocketRpcRequestParser, ParsesSessionHandshakeSubscriptionAndSnapshot)
{
    auto const hello = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":50,"method":"session.hello","params":{"clientId":"editor:one","localCapability":"secret"}})");
    auto const* hello_request = std::get_if<iv::SessionHelloRequest>(
        &hello.payload);
    ASSERT_NE(hello_request, nullptr);
    EXPECT_EQ(hello_request->client_id, "editor:one");
    EXPECT_EQ(hello_request->local_capability, "secret");

    auto const subscribe = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":51,"method":"session.subscribe","params":{"afterRevision":18446744073709551615,"topics":["graph.nodesUpdated"]}})");
    auto const* subscribe_request = std::get_if<iv::SessionSubscribeRequest>(
        &subscribe.payload);
    ASSERT_NE(subscribe_request, nullptr);
    EXPECT_EQ(
        subscribe_request->after_revision,
        std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(
        subscribe_request->topics,
        std::vector<std::string>{"graph.nodesUpdated"});

    auto const snapshot = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":52,"method":"session.snapshot","params":{}})");
    EXPECT_NE(
        std::get_if<iv::SessionSnapshotRequest>(&snapshot.payload), nullptr);
}

TEST(SocketRpcRequestParser, PreservesExpectedRevisionOnMutation)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":53,"method":"nodeInstances.delete","params":{"instanceId":"instance:one","expectedRevision":17}})");

    EXPECT_EQ(parsed.expected_revision, 17u);
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

TEST(SocketRpcRequestParser, ParsesTypedNodeMessageDispatch)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":41,"method":"nodeMessages.dispatch","params":{"target":{"instanceId":"instance:voice","path":[{"kind":"virtualNode","sourceIdentity":"filter"},{"kind":"virtualMember","index":2}]},"eventTypeNominalId":"c:@S@SetCutoff","eventTypeDefinitionFingerprint":"0123456789abcdef","payload":{"cutoff":1800.0}}})");

    auto const* request = std::get_if<iv::DispatchNodeMessageRequest>(
        &parsed.payload);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(parsed.request_id, 41);
    EXPECT_EQ(request->target.instance_id, "instance:voice");
    ASSERT_EQ(request->target.path.size(), 2u);
    auto const* member = std::get_if<iv::ProjectVirtualMemberSelector>(
        &request->target.path[1]);
    ASSERT_NE(member, nullptr);
    EXPECT_EQ(member->index, 2u);
    EXPECT_EQ(request->event_type_nominal_id, "c:@S@SetCutoff");
    EXPECT_EQ(
        request->event_type_definition_fingerprint,
        "0123456789abcdef");
    ASSERT_NE(request->payload, nullptr);
    EXPECT_EQ((*request->payload)["cutoff"], 1800.0);
}

TEST(SocketRpcRequestParser, ParsesNodeEventProtocolQuery)
{
    auto const parsed = iv::parse_socket_rpc_request(
        R"({"jsonrpc":"2.0","id":43,"method":"nodeMessages.getProtocol","params":{}})");

    EXPECT_EQ(parsed.request_id, 43);
    EXPECT_NE(
        std::get_if<iv::GetNodeEventProtocolRequest>(&parsed.payload),
        nullptr);
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

TEST(SocketRpcSessionProjectStateResultBuilder, SerializesCanonicalCommands)
{
    iv::SocketRpcSessionProjectStateResultBuilder builder;
    builder.succeed({iv::ProjectCommand{
        .command = "graphConnections.upsert",
        .args = Json{{"connection_id", "sample:main"}},
    }});

    auto const response = parse_json_line(builder.build(54));

    ASSERT_EQ(response["result"]["commands"].size(), 1u);
    EXPECT_EQ(
        response["result"]["commands"][0]["command"],
        "graphConnections.upsert");
    EXPECT_EQ(
        response["result"]["commands"][0]["args"]["connection_id"],
        "sample:main");
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

TEST(SocketRpcNodeMessageResultBuilder, SerializesSuccessfulDiagnostics)
{
    iv::SocketRpcNodeMessageResultBuilder builder;
    builder.succeed({iv::SocketRpcNodeMessageDiagnostic{
        .target = iv::NodeAddress{
            .instance_id = "instance:voice",
            .path = {iv::ProjectTiledChildSelector{.name = "right"}},
        },
        .message = "selected concrete node does not implement the event",
    }});

    auto const response = parse_json_line(builder.build(42));

    EXPECT_EQ(response["result"]["ok"], true);
    ASSERT_EQ(response["result"]["diagnostics"].size(), 1u);
    EXPECT_EQ(
        response["result"]["diagnostics"][0]["target"]["instanceId"],
        "instance:voice");
    EXPECT_EQ(
        response["result"]["diagnostics"][0]["target"]["path"][0]["name"],
        "right");
}

TEST(SocketRpcNodeEventProtocolResultBuilder, SerializesNeutralSchemasAndReceivers)
{
    iv::SocketRpcNodeEventProtocolResultBuilder builder;
    auto field = std::make_shared<iv::NodeEventValueSchema const>(
        iv::NodeEventValueSchema{
            .kind = iv::NodeEventValueKind::floating_point,
            .integer_bit_width = 64,
        });
    builder.succeed(iv::NodeEventProtocolSnapshot{
        .project_graph_generation = 12,
        .definitions_generation = 7,
        .event_types = {{
            .id = {
                .nominal_id = "c:@S@SetCutoff",
                .definition_fingerprint = "event-v2",
            },
            .display_name = "SetCutoff",
            .payload = {
                .kind = iv::NodeEventValueKind::aggregate,
                .fields = {{.name = "cutoff", .value = std::move(field)}},
            },
        }},
        .definitions = {{
            .definition_id = "iv.example.filter",
            .supported_event_types = {{
                .nominal_id = "c:@S@SetCutoff",
                .definition_fingerprint = "event-v2",
            }},
        }},
        .receivers = {{
            .address = {.instance_id = "instance:filter"},
            .supported_event_types = {{
                .nominal_id = "c:@S@SetCutoff",
                .definition_fingerprint = "event-v2",
            }},
        }},
    });

    auto const response = parse_json_line(builder.build(44));
    auto const& protocol = response["result"];
    EXPECT_EQ(protocol["formatVersion"], 0);
    EXPECT_EQ(protocol["projectGraphGeneration"], 12);
    EXPECT_EQ(protocol["definitionsGeneration"], 7);
    ASSERT_EQ(protocol["eventTypes"].size(), 1u);
    EXPECT_EQ(protocol["eventTypes"][0]["nominalId"], "c:@S@SetCutoff");
    EXPECT_EQ(protocol["eventTypes"][0]["payload"]["kind"], "aggregate");
    EXPECT_EQ(
        protocol["eventTypes"][0]["payload"]["fields"][0]["value"]["kind"],
        "floatingPoint");
    ASSERT_EQ(protocol["definitions"].size(), 1u);
    EXPECT_EQ(
        protocol["definitions"][0]["definitionId"],
        "iv.example.filter");
    ASSERT_EQ(protocol["receivers"].size(), 1u);
    EXPECT_EQ(
        protocol["receivers"][0]["address"]["instanceId"],
        "instance:filter");
    auto const serialized = protocol.dump();
    EXPECT_EQ(serialized.find("NodeCodeKey"), std::string::npos);
    EXPECT_EQ(serialized.find("byteOffset"), std::string::npos);
    EXPECT_EQ(serialized.find("llvm"), std::string::npos);
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
        .definition_id = "iv.test.oscillator",
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
    EXPECT_EQ(node["definitionId"], "iv.test.oscillator");
}









TEST(SocketRpcCreateNodeInstanceResultBuilder, SerializesCreatedInstanceId)
{
    iv::SocketRpcCreateNodeInstanceResultBuilder builder;
    builder.succeed("instance:7");

    auto const response = parse_json_line(builder.build(17));

    EXPECT_EQ(response["id"], 17);
    EXPECT_EQ(response["result"]["instanceId"], "instance:7");
}

TEST(SocketRpcIvPackageDefinitionsResultBuilder, SerializesProvenance)
{
    iv::SocketRpcIvPackageDefinitionsResultBuilder builder;
    builder.succeed({
        iv::IvPackageInfo{
            .package_id = "iv.builtin",
            .package_root = "/tmp/builtin",
            .provenance = iv::PackageProvenance::built_in,
        },
        iv::IvPackageInfo{
            .package_id = "iv.common",
            .package_root = "/tmp/common",
            .provenance = iv::PackageProvenance::common,
        },
        iv::IvPackageInfo{
            .package_id = "iv.project",
            .package_root = "/tmp/project",
            .provenance = iv::PackageProvenance::project_local,
        },
    });

    auto const response = parse_json_line(builder.build(19));
    auto const& packages = response["result"]["packages"];
    ASSERT_EQ(packages.size(), 3u);
    EXPECT_EQ(packages[0]["provenance"], "builtIn");
    EXPECT_EQ(packages[1]["provenance"], "common");
    EXPECT_EQ(packages[2]["provenance"], "projectLocal");
    EXPECT_FALSE(packages[0].contains("projectLocal"));
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
