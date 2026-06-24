#include <intravenous/runtime/runtime_project_timeline_execution_bridge.h>

#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_events.h>
#include <intravenous/runtime/timeline_execution.h>

namespace iv {
namespace {
Timeline *bound_timeline = nullptr;
TimelineExecution *bound_timeline_execution = nullptr;
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
    ProjectSetTimelineCompiledSampleCacheChunkSizeMultiplierRequest const &request,
    ProjectAckBuilder &builder)
{
    if (bound_timeline_execution == nullptr) {
        return;
    }
    bound_timeline_execution->set_compiled_sample_cache_chunk_size_multiplier(
        request.compiled_sample_cache_chunk_size_multiplier);
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_override_settings(ProjectOverrideSettingsRequest const &request)
{
    if (bound_timeline_execution == nullptr) {
        return;
    }
    if (!request.compiled_sample_cache_chunk_size_multiplier.has_value()) {
        return;
    }
    bound_timeline_execution->set_compiled_sample_cache_chunk_size_multiplier(
        *request.compiled_sample_cache_chunk_size_multiplier);
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_set_timeline_lane_sample_channel_type(
    ProjectSetTimelineLaneSampleChannelTypeRequest const &request,
    ProjectAckBuilder &builder)
{
    if (bound_timeline == nullptr) {
        return;
    }

    auto const lane = bound_timeline->resolve_public_lane_id(request.lane_id);
    if (!lane.has_value()) {
        throw std::runtime_error("timeline lane not found");
    }
    bound_timeline->with_graph([&](LaneGraph &graph) {
        auto &record = graph.lane(*lane);
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
            .public_id_for_lane = [timeline = bound_timeline](LaneId lane_id) {
                return timeline->lane_public_id(lane_id);
            },
            .outputs_for_lanes = [timeline = bound_timeline](std::vector<LaneId> const &lanes) {
                return outputs_for_lanes(*timeline, lanes);
            },
            .visit_lanes = [timeline = bound_timeline](std::vector<LaneId> const &lanes, TimelineLaneVisitFn const &visit) {
                timeline->with_graph([&](LaneGraph const &graph) {
                    for (auto const lane_id : lanes) {
                        if (!graph.contains(lane_id)) {
                            continue;
                        }
                        auto const &record = graph.lane(lane_id);
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
            .changed_lanes = {*lane},
        });
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_connect_timeline_lanes(
    ProjectConnectTimelineLanesRequest const &request,
    ProjectAckBuilder &builder)
{
    if (bound_timeline == nullptr) {
        return;
    }

    auto const source = bound_timeline->resolve_public_lane_id(request.source_lane_id);
    auto const target = bound_timeline->resolve_public_lane_id(request.target_lane_id);
    bound_timeline->with_graph([&](LaneGraph &graph) {
        if (!source.has_value() || !graph.contains(*source)) {
            throw std::runtime_error("timeline source lane not found");
        }
        if (!target.has_value() || !graph.contains(*target)) {
            throw std::runtime_error("timeline target lane not found");
        }
        graph.connect(
            *source,
            *target,
            LanePortId{
                .kind = request.port_kind,
                .ordinal = request.port_ordinal,
            });
    });
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

IV_SUBSCRIBE_LINKER_EVENT(
    ProjectSetTimelineCompiledSampleCacheChunkSizeMultiplierRequestedEvent,
    iv_runtime_project_set_timeline_compiled_sample_cache_chunk_size_multiplier_requested_event,
    handle_set_timeline_compiled_sample_cache_chunk_size);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectSetTimelineLaneSampleChannelTypeRequestedEvent,
    iv_runtime_project_set_timeline_lane_sample_channel_type_requested_event,
    handle_set_timeline_lane_sample_channel_type);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectConnectTimelineLanesRequestedEvent,
    iv_runtime_project_connect_timeline_lanes_requested_event,
    handle_connect_timeline_lanes);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectOverrideSettingsRequestedEvent,
    iv_runtime_project_override_settings_requested_event,
    handle_override_settings);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectPersistenceCollectStateEvent,
    iv_runtime_project_persistence_collect_state_event,
    [](ProjectPersistenceBuilder &builder) {
        if (bound_timeline == nullptr || bound_timeline_execution == nullptr) {
            return;
        }
        builder.add_project_compiled_sample_cache_chunk_size_multiplier(
            bound_timeline_execution->compiled_sample_cache_chunk_size_multiplier());
        std::vector<ProjectSetTimelineLaneSampleChannelTypeRequest> lane_sample_channel_types;
        for (auto const lane : bound_timeline->lane_ids()) {
            auto const channel_type = bound_timeline->lane_sample_channel_type(lane);
            if (!channel_type.has_value()) {
                continue;
            }
            lane_sample_channel_types.push_back(ProjectSetTimelineLaneSampleChannelTypeRequest{
                .lane_id = bound_timeline->lane_public_id(lane),
                .sample_channel_type = *channel_type,
            });
        }
        builder.add_lane_sample_channel_types(std::move(lane_sample_channel_types));
        std::vector<ProjectConnectTimelineLanesRequest> lane_connections;
        for (auto const &connection : bound_timeline->lane_connections()) {
            lane_connections.push_back(ProjectConnectTimelineLanesRequest{
                .source_lane_id = bound_timeline->lane_public_id(connection.source),
                .target_lane_id = bound_timeline->lane_public_id(connection.target),
                .port_kind = connection.input.kind,
                .port_ordinal = connection.input.ordinal,
            });
        }
        builder.add_lane_connections(std::move(lane_connections));
    });
} // namespace

void bind_runtime_project_timeline_execution_bridge(
    Timeline &timeline,
    TimelineExecution &timeline_execution,
    std::filesystem::path workspace_root)
{
    bound_timeline = &timeline;
    bound_timeline_execution = &timeline_execution;
    (void)workspace_root;
}

void unbind_runtime_project_timeline_execution_bridge(
    TimelineExecution const &timeline_execution)
{
    if (bound_timeline_execution == &timeline_execution) {
        bound_timeline = nullptr;
        bound_timeline_execution = nullptr;
    }
}
} // namespace iv
