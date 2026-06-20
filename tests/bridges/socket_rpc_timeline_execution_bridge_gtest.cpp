#include "../module_test_utils.h"

#include <intravenous/basic_lane_nodes/controls.h>
#include <intravenous/runtime/socket_rpc_timeline_execution_bridge.h>
#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_execution.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string_view>

namespace {
using Json = nlohmann::ordered_json;
using iv::test_support::mutable_module_fixture_workspace;
using iv::test_support::read_text;
using iv::test_support::write_text;

Json parse_json_line(std::string_view line)
{
    return Json::parse(line);
}
} // namespace

TEST(SocketRpcTimelineExecutionBridge, UnboundSetChunkSizeReturnsAck)
{
    iv::SocketRpcAckResponseBuilder builder;

    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_set_timeline_compiled_sample_cache_chunk_size_multiplier_event,
        iv::SetTimelineCompiledSampleCacheChunkSizeMultiplierRequest {
            .compiled_sample_cache_chunk_size_multiplier = 8,
        },
        builder);

    auto const response = parse_json_line(builder.build(1));
    EXPECT_EQ(response["result"]["ok"], true);
}

TEST(SocketRpcTimelineExecutionBridge, BoundSetChunkSizeUpdatesTimelineExecution)
{
    iv::Timeline timeline;
    iv::TimelineExecution execution(8, 16);
    auto const workspace = mutable_module_fixture_workspace(
        "socket_rpc_timeline_execution_bridge",
        "local_cmake");
    write_text(
        workspace / ".intravenous",
        "block_size=8\n"
        "compiled_sample_cache_chunk_size_multiplier=16\n");

    iv::bind_socket_rpc_timeline_execution_bridge(timeline, execution, workspace);

    iv::SocketRpcAckResponseBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_set_timeline_compiled_sample_cache_chunk_size_multiplier_event,
        iv::SetTimelineCompiledSampleCacheChunkSizeMultiplierRequest {
            .compiled_sample_cache_chunk_size_multiplier = 8,
        },
        builder);

    auto const response = parse_json_line(builder.build(1));
    EXPECT_EQ(response["result"]["ok"], true);
    EXPECT_EQ(execution.compiled_sample_cache_chunk_size_multiplier(), 8u);
    EXPECT_TRUE(read_text(workspace / ".intravenous").contains(
        "compiled_sample_cache_chunk_size_multiplier=8"));

    iv::unbind_socket_rpc_timeline_execution_bridge(execution);
}

TEST(SocketRpcTimelineExecutionBridge, BoundSetChunkSizeRejectsZeroMultiplier)
{
    iv::Timeline timeline;
    iv::TimelineExecution execution(8, 16);
    auto const workspace = mutable_module_fixture_workspace(
        "socket_rpc_timeline_execution_bridge_invalid",
        "local_cmake");
    write_text(
        workspace / ".intravenous",
        "block_size=8\n"
        "compiled_sample_cache_chunk_size_multiplier=16\n");

    iv::bind_socket_rpc_timeline_execution_bridge(timeline, execution, workspace);

    iv::SocketRpcAckResponseBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_set_timeline_compiled_sample_cache_chunk_size_multiplier_event,
        iv::SetTimelineCompiledSampleCacheChunkSizeMultiplierRequest {
            .compiled_sample_cache_chunk_size_multiplier = 0,
        },
        builder);

    auto const response = parse_json_line(builder.build(1));
    EXPECT_EQ(
        response["error"]["message"].get<std::string>(),
        "compiled sample cache chunk size multiplier must be > 0");
    EXPECT_EQ(execution.compiled_sample_cache_chunk_size_multiplier(), 16u);
    EXPECT_TRUE(read_text(workspace / ".intravenous").contains(
        "compiled_sample_cache_chunk_size_multiplier=16"));

    iv::unbind_socket_rpc_timeline_execution_bridge(execution);
}

TEST(SocketRpcTimelineExecutionBridge, BoundSetLaneSampleChannelTypeUpdatesTimeline)
{
    iv::Timeline timeline;
    auto const lane = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(iv::TypeErasedLaneNode(iv::KnobLaneNode{ .value = 1.0f }));
    });
    iv::TimelineExecution execution(8, 16);
    auto const workspace = mutable_module_fixture_workspace(
        "socket_rpc_timeline_execution_bridge_lane_channel_type",
        "local_cmake");
    write_text(
        workspace / ".intravenous",
        "block_size=8\n"
        "compiled_sample_cache_chunk_size_multiplier=16\n");

    iv::bind_socket_rpc_timeline_execution_bridge(timeline, execution, workspace);

    iv::SocketRpcAckResponseBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_set_timeline_lane_sample_channel_type_event,
        iv::SetTimelineLaneSampleChannelTypeRequest{
            .lane_id = lane.value,
            .sample_channel_type = iv::ChannelTypeId::mono,
        },
        builder);

    auto const response = parse_json_line(builder.build(1));
    EXPECT_EQ(response["result"]["ok"], true);
    EXPECT_EQ(timeline.lane_sample_channel_type(lane), iv::ChannelTypeId::mono);

    iv::unbind_socket_rpc_timeline_execution_bridge(execution);
}
