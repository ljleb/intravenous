#include "../module_test_utils.h"

#include <intravenous/basic_lane_nodes/controls.h>
#include <intravenous/runtime/project_persistence.h>
#include <intravenous/runtime/runtime_project_api_types.h>
#include <intravenous/runtime/runtime_project_timeline_execution_bridge.h>
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

struct ProjectNotificationWitness {
    std::vector<iv::ProjectMessageNotification> messages {};
};

ProjectNotificationWitness *g_project_notification_witness = nullptr;

IV_SUBSCRIBE_LINKER_EVENT(
    iv::ProjectNotificationEvent,
    iv_runtime_project_notification_event,
    +[](iv::ProjectNotification const &notification) {
        if (g_project_notification_witness == nullptr) {
            return;
        }
        if (auto const *message =
                std::get_if<iv::ProjectMessageNotification>(&notification)) {
            g_project_notification_witness->messages.push_back(*message);
        }
    });

Json parse_json_line(std::string_view line)
{
    return Json::parse(line);
}

std::string settings_line(size_t multiplier)
{
    return Json{
        {"command", "project.overrideSettings"},
        {"args", Json{
            {"compiled_sample_cache_chunk_size_multiplier", multiplier},
        }},
    }.dump() + "\n";
}
} // namespace

TEST(SocketRpcTimelineExecutionBridge, UnboundSetChunkSizeReturnsError)
{
    iv::SocketRpcAckResponseBuilder builder;

    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_set_timeline_compiled_sample_cache_chunk_size_multiplier_event,
        iv::SetTimelineCompiledSampleCacheChunkSizeMultiplierRequest {
            .compiled_sample_cache_chunk_size_multiplier = 8,
        },
        builder);

    auto const response = parse_json_line(builder.build(1));
    EXPECT_EQ(
        response["error"]["message"].get<std::string>(),
        "runtime project event was not handled");
}

TEST(SocketRpcTimelineExecutionBridge, BoundSetChunkSizeUpdatesTimelineExecution)
{
    iv::Timeline timeline;
    iv::TimelineExecution execution(8, 16);
    auto const workspace = mutable_module_fixture_workspace(
        "socket_rpc_timeline_execution_bridge",
        "local_cmake");
    write_text(workspace / "project.intravenous", settings_line(16));

    iv::IvModuleInstances iv_module_instances;
    iv::GraphInputLanes graph_input_lanes;
    iv::AudioDeviceLanes audio_device_lanes(
        48000,
        8,
        iv::AudioDeviceLanesBackend{
            .list_output_devices = [] { return std::vector<iv::AudioDeviceDescriptor>{}; },
            .list_input_devices = [] { return std::vector<iv::AudioDeviceDescriptor>{}; },
            .make_output_device = [](std::string const &, iv::RenderConfig const &) -> iv::AudioOutputDevice {
                throw std::runtime_error("unexpected output device creation");
            },
            .make_input_device = [](std::string const &, iv::RenderConfig const &) -> iv::AudioInputDevice {
                throw std::runtime_error("unexpected input device creation");
            },
        });
    iv::LaneViews lane_views;
    iv::ProjectPersistence persistence(
        workspace,
        iv::StartupConfigState{
            .workspace_root = workspace,
            .execution = iv::RuntimeExecutionConfig{
                .sample_rate = 48000,
                .block_size = 8,
                .compiled_sample_cache_chunk_size_multiplier = 16,
            },
        });
    iv::bind_runtime_project_timeline_execution_bridge(timeline, execution, workspace);
    iv::bind_project_persistence_bridge(persistence);
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
    EXPECT_FALSE(read_text(workspace / "project.intravenous").contains(
        "\"compiled_sample_cache_chunk_size_multiplier\":8"));

    iv::SocketRpcAckResponseBuilder save_builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_save_project_event,
        iv::SaveProjectRequest{},
        save_builder);
    auto const save_response = parse_json_line(save_builder.build(2));
    EXPECT_EQ(save_response["result"]["ok"], true);
    EXPECT_TRUE(read_text(workspace / "project.intravenous").contains(
        "\"compiled_sample_cache_chunk_size_multiplier\":8"));

    iv::unbind_project_persistence_bridge(persistence);
    iv::unbind_runtime_project_timeline_execution_bridge(execution);
    iv::unbind_socket_rpc_timeline_execution_bridge(execution);
}

TEST(SocketRpcTimelineExecutionBridge, BoundSetChunkSizeRejectsZeroMultiplier)
{
    iv::Timeline timeline;
    iv::TimelineExecution execution(8, 16);
    auto const workspace = mutable_module_fixture_workspace(
        "socket_rpc_timeline_execution_bridge_invalid",
        "local_cmake");
    write_text(workspace / "project.intravenous", settings_line(16));

    iv::bind_runtime_project_timeline_execution_bridge(timeline, execution, workspace);
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
    EXPECT_TRUE(read_text(workspace / "project.intravenous").contains(
        "\"compiled_sample_cache_chunk_size_multiplier\":16"));

    iv::unbind_runtime_project_timeline_execution_bridge(execution);
    iv::unbind_socket_rpc_timeline_execution_bridge(execution);
}

TEST(SocketRpcTimelineExecutionBridge, BoundSetLaneSampleChannelTypeUpdatesTimeline)
{
    iv::Timeline timeline;
    auto const lane = timeline.with_graph([&](iv::LaneGraph& graph) {
        return graph.add_lane(iv::TypeErasedLaneNode(iv::KnobLaneNode{ .value = 1.0f }));
    });
    auto const public_lane_id = timeline.lane_public_id(lane);
    iv::TimelineExecution execution(8, 16);
    auto const workspace = mutable_module_fixture_workspace(
        "socket_rpc_timeline_execution_bridge_lane_channel_type",
        "local_cmake");
    write_text(workspace / "project.intravenous", settings_line(16));

    iv::bind_runtime_project_timeline_execution_bridge(timeline, execution, workspace);
    iv::bind_socket_rpc_timeline_execution_bridge(timeline, execution, workspace);

    iv::SocketRpcAckResponseBuilder builder;
    IV_INVOKE_LINKER_EVENT(
        iv::iv_socket_rpc_set_timeline_lane_sample_channel_type_event,
        iv::SetTimelineLaneSampleChannelTypeRequest{
            .lane_id = public_lane_id,
            .sample_channel_type = iv::ChannelTypeId::mono,
        },
        builder);

    auto const response = parse_json_line(builder.build(1));
    EXPECT_EQ(response["result"]["ok"], true);
    EXPECT_EQ(timeline.lane_sample_channel_type(lane), iv::ChannelTypeId::mono);

    iv::unbind_runtime_project_timeline_execution_bridge(execution);
    iv::unbind_socket_rpc_timeline_execution_bridge(execution);
}

TEST(SocketRpcTimelineExecutionBridge, DirectOverrideSettingsEventUpdatesTimelineExecution)
{
    iv::Timeline timeline;
    iv::TimelineExecution execution(8, 16);
    auto const workspace = mutable_module_fixture_workspace(
        "socket_rpc_timeline_execution_bridge_direct_override",
        "local_cmake");

    iv::bind_runtime_project_timeline_execution_bridge(timeline, execution, workspace);

    IV_INVOKE_LINKER_EVENT(
        iv::iv_runtime_project_override_settings_requested_event,
        iv::ProjectOverrideSettingsRequest{
            .args = Json{
                {"compiled_sample_cache_chunk_size_multiplier", 8},
            },
            .workspace_root = workspace,
            .startup = iv::StartupConfigState{
                .workspace_root = workspace,
                .execution = iv::RuntimeExecutionConfig{
                    .sample_rate = 48000,
                    .block_size = 8,
                    .compiled_sample_cache_chunk_size_multiplier = 16,
                },
            },
        });

    EXPECT_EQ(execution.compiled_sample_cache_chunk_size_multiplier(), 8u);

    iv::unbind_runtime_project_timeline_execution_bridge(execution);
}

TEST(SocketRpcTimelineExecutionBridge, LoadAppliesRecognizedOverrideSettingsAndLogsUnknownOnes)
{
    iv::Timeline timeline;
    iv::TimelineExecution execution(8, 16);
    auto const workspace = mutable_module_fixture_workspace(
        "socket_rpc_timeline_execution_bridge_unknown_override",
        "local_cmake");
    write_text(
        workspace / "project.intravenous",
        Json{
            {"command", "project.overrideSettings"},
            {"args", Json{
                {"compiled_sample_cache_chunk_size_multiplier", 8},
                {"unknown_setting", "mystery"},
            }},
        }.dump() + "\n");

    iv::ProjectPersistence persistence(
        workspace,
        iv::StartupConfigState{
            .workspace_root = workspace,
            .execution = iv::RuntimeExecutionConfig{
                .sample_rate = 48000,
                .block_size = 8,
                .compiled_sample_cache_chunk_size_multiplier = 16,
            },
        });
    ProjectNotificationWitness witness;
    g_project_notification_witness = &witness;

    iv::bind_runtime_project_timeline_execution_bridge(timeline, execution, workspace);
    iv::bind_project_persistence_bridge(persistence);
    persistence.load();

    EXPECT_EQ(execution.compiled_sample_cache_chunk_size_multiplier(), 8u);
    ASSERT_EQ(witness.messages.size(), 1u);
    EXPECT_EQ(witness.messages.front().level, "warning");
    EXPECT_TRUE(witness.messages.front().message.contains("unknown_setting"));

    iv::unbind_project_persistence_bridge(persistence);
    iv::unbind_runtime_project_timeline_execution_bridge(execution);
    g_project_notification_witness = nullptr;
}
