#include <intravenous/runtime/socket_rpc_timeline_execution_bridge.h>

#include <intravenous/runtime/config.h>
#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_events.h>
#include <intravenous/runtime/timeline_execution.h>

namespace iv {
namespace {
Timeline *bound_timeline = nullptr;
TimelineExecution *bound_timeline_execution = nullptr;
std::filesystem::path bound_workspace_root {};
std::uint64_t timeline_change_version_index = 1;

std::vector<TimelineLaneOutputs> outputs_for_lanes(
    Timeline &timeline,
    std::vector<LaneId> const &lanes)
{
    std::vector<TimelineLaneOutputs> results;
    results.reserve(lanes.size());
    for (auto const lane : lanes) {
        results.push_back(TimelineLaneOutputs{
            .lane = lane,
            .outputs = timeline.lane_outputs_for(lane),
        });
    }
    return results;
}

void handle_set_timeline_compiled_sample_cache_chunk_size(
    SetTimelineCompiledSampleCacheChunkSizeMultiplierRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    if (bound_timeline_execution == nullptr) {
        return;
    }
    try {
        bound_timeline_execution->set_compiled_sample_cache_chunk_size_multiplier(
            request.compiled_sample_cache_chunk_size_multiplier);
        set_runtime_project_compiled_sample_cache_chunk_size_multiplier(
            bound_workspace_root,
            request.compiled_sample_cache_chunk_size_multiplier);
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

void handle_set_timeline_lane_sample_channel_type(
    SetTimelineLaneSampleChannelTypeRequest const &request,
    SocketRpcAckResponseBuilder &builder)
{
    if (bound_timeline == nullptr) {
        return;
    }
    try {
        auto const lane = LaneId{request.lane_id};
        bound_timeline->with_graph([&](LaneGraph& graph) {
            if (!graph.contains(lane)) {
                throw std::runtime_error("timeline lane not found");
            }
            auto& record = graph.lane(lane);
            if (!record.sample_channel_type.has_value()) {
                throw std::runtime_error("timeline lane does not produce samples");
            }
            record.sample_channel_type = request.sample_channel_type;
        });

        IV_INVOKE_LINKER_EVENT(
            iv_runtime_timeline_lanes_changed_event,
            TimelineLanesChanged{
                .version_index = timeline_change_version_index++,
                .lane_set_changed = false,
                .metadata_for_lane = [timeline = bound_timeline](LaneId lane_id) {
                    return timeline->lane_metadata(lane_id);
                },
                .outputs_for_lanes = [timeline = bound_timeline](std::vector<LaneId> const &lanes) {
                    return outputs_for_lanes(*timeline, lanes);
                },
                .visit_lanes = [timeline = bound_timeline](std::vector<LaneId> const &lanes, TimelineLaneVisitFn const &visit) {
                    timeline->with_graph([&](LaneGraph const& graph) {
                        for (auto const lane_id : lanes) {
                            if (!graph.contains(lane_id)) {
                                continue;
                            }
                            auto const& record = graph.lane(lane_id);
                            visit(
                                lane_id,
                                record.node,
                                record.output,
                                record.sample_channel_type,
                                graph.inputs_for(lane_id),
                                record.external_task_dependencies);
                        }
                    });
                },
                .changed_lanes = { lane },
            });
    } catch (std::exception const &e) {
        builder.fail(e.what());
    }
}

IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSetTimelineCompiledSampleCacheChunkSizeMultiplierEvent,
    iv_socket_rpc_set_timeline_compiled_sample_cache_chunk_size_multiplier_event,
    handle_set_timeline_compiled_sample_cache_chunk_size);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSetTimelineLaneSampleChannelTypeEvent,
    iv_socket_rpc_set_timeline_lane_sample_channel_type_event,
    handle_set_timeline_lane_sample_channel_type);
} // namespace

void bind_socket_rpc_timeline_execution_bridge(
    Timeline &timeline,
    TimelineExecution &timeline_execution,
    std::filesystem::path workspace_root)
{
    bound_timeline = &timeline;
    bound_timeline_execution = &timeline_execution;
    bound_workspace_root = std::move(workspace_root);
}

void unbind_socket_rpc_timeline_execution_bridge(
    TimelineExecution const &timeline_execution)
{
    if (bound_timeline_execution == &timeline_execution) {
        bound_timeline = nullptr;
        bound_timeline_execution = nullptr;
        bound_workspace_root.clear();
    }
}
} // namespace iv
