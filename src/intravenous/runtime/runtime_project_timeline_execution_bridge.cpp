#include <intravenous/runtime/runtime_project_timeline_execution_bridge.h>

#include <intravenous/runtime/authored_lanes.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_events.h>
#include <intravenous/runtime/timeline_execution.h>

#include <algorithm>
#include <unordered_set>

namespace iv {
namespace {
Timeline *bound_timeline = nullptr;
TimelineExecution *bound_timeline_execution = nullptr;
AuthoredLanes *bound_authored_lanes = nullptr;
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

void emit_lane_ui_model_changed(LaneId changed_lane)
{
    if (bound_timeline == nullptr) {
        return;
    }
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_timeline_lanes_changed_event,
        TimelineLanesChanged{
            .version_index = timeline_change_version_index++,
            .lane_set_changed = false,
            .metadata_for_lane = [timeline = bound_timeline](LaneId lane) {
                return timeline->lane_metadata(lane);
            },
            .model_type_id_for_lane = [timeline = bound_timeline](LaneId lane) {
                return timeline->lane_model_type_id(lane);
            },
            .public_id_for_lane = [timeline = bound_timeline](LaneId lane) {
                return timeline->lane_public_id(lane);
            },
            .outputs_for_lanes = [timeline = bound_timeline](std::vector<LaneId> const &lanes) {
                return outputs_for_lanes(*timeline, lanes);
            },
            .visit_lanes = [timeline = bound_timeline](std::vector<LaneId> const &lanes, TimelineLaneVisitFn const &visit) {
                timeline->with_graph([&](LaneGraph const &graph) {
                    for (auto const lane : lanes) {
                        if (!graph.contains(lane)) continue;
                        auto const &record = graph.lane(lane);
                        visit(lane, record.node, record.output, record.sample_channel_type,
                            graph.inputs_for(lane), record.external_task_dependencies);
                    }
                });
            },
            .changed_lanes = {changed_lane},
        });
}

void emit_authored_lane_created(LaneId created_lane)
{
    if (bound_timeline == nullptr) return;
    IV_INVOKE_LINKER_EVENT(iv_runtime_timeline_lanes_changed_event, TimelineLanesChanged{
        .version_index = timeline_change_version_index++, .lane_set_changed = true,
        .metadata_for_lane = [timeline = bound_timeline](LaneId lane) { return timeline->lane_metadata(lane); },
        .model_type_id_for_lane = [timeline = bound_timeline](LaneId lane) { return timeline->lane_model_type_id(lane); },
        .public_id_for_lane = [timeline = bound_timeline](LaneId lane) { return timeline->lane_public_id(lane); },
        .outputs_for_lanes = [timeline = bound_timeline](std::vector<LaneId> const& lanes) { return outputs_for_lanes(*timeline, lanes); },
        .visit_lanes = [timeline = bound_timeline](std::vector<LaneId> const& lanes, TimelineLaneVisitFn const& visit) {
            timeline->with_graph([&](LaneGraph const& graph) {
                for (auto const lane : lanes) if (graph.contains(lane)) {
                    auto const& record = graph.lane(lane);
                    visit(lane, record.node, record.output, record.sample_channel_type,
                        graph.inputs_for(lane), record.external_task_dependencies);
                }
            });
        },
        .created_lanes = {created_lane},
    });
}

std::vector<LaneId> affected_lanes_from_connection_delta(
    std::vector<LaneGraphConnection> const &before,
    std::vector<LaneGraphConnection> const &after)
{
    auto contains = [](std::vector<LaneGraphConnection> const &connections, LaneGraphConnection const &candidate) {
        return std::find(connections.begin(), connections.end(), candidate) != connections.end();
    };

    std::unordered_set<std::uint64_t> lane_values;
    for (auto const &connection : after) {
        if (!contains(before, connection)) {
            lane_values.insert(connection.source.value);
            lane_values.insert(connection.target.value);
        }
    }
    for (auto const &connection : before) {
        if (!contains(after, connection)) {
            lane_values.insert(connection.source.value);
            lane_values.insert(connection.target.value);
        }
    }

    std::vector<LaneId> lanes;
    lanes.reserve(lane_values.size());
    for (auto const value : lane_values) {
        lanes.push_back(LaneId{value});
    }
    return lanes;
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
            .model_type_id_for_lane = [timeline = bound_timeline](LaneId lane_id) {
                return timeline->lane_model_type_id(lane_id);
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

void handle_set_timeline_lane_ui_state(
    ProjectSetTimelineLaneUiStateRequest const &request,
    ProjectAckBuilder &builder)
{
    if (bound_timeline == nullptr) return;
    auto const lane = bound_timeline->resolve_public_lane_id(request.lane_id);
    if (!lane.has_value()) throw std::runtime_error("timeline lane not found");

    auto const result = bound_timeline->apply_lane_ui_state(*lane, LaneUiStateWrite{
        .expected_revision = request.expected_revision,
        .serialized_state = request.serialized_state,
    });
    if (!result.accepted) {
        throw std::runtime_error(result.error_message.empty()
            ? "timeline lane rejected UI state" : result.error_message);
    }

    if (bound_authored_lanes != nullptr && bound_authored_lanes->contains(request.lane_id)) {
        auto const snapshot = bound_timeline->lane_ui_state_snapshot(*lane);
        if (!snapshot.has_value()) throw std::runtime_error("authored lane did not provide canonical UI state");
        bound_authored_lanes->update_canonical_state(request.lane_id, snapshot->serialized_state);
    }

    if (result.effect != LaneUiStateEffect::ui_only) {
        emit_lane_ui_model_changed(*lane);
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void handle_get_timeline_lane_types(ProjectLaneTypesBuilder &builder)
{
    if (bound_authored_lanes == nullptr) return;
    builder.succeed(AuthoredLanes::creatable_lane_types());
}

void handle_create_timeline_lane(ProjectCreateTimelineLaneRequest const& request, ProjectAckBuilder &builder)
{
    if (bound_timeline == nullptr || bound_authored_lanes == nullptr) return;
    auto batch = request.lane_id.has_value()
        ? bound_authored_lanes->reload(AuthoredLaneRecord{
            .lane_id = *request.lane_id,
            .type_id = request.type_id,
            .serialized_state = request.serialized_state.value_or("")})
        : bound_authored_lanes->create(request.type_id);
    if (batch.upserts.size() != 1) throw std::runtime_error("authored lane creation did not produce one lane");
    auto const lane = batch.upserts.front().lane;
    bound_timeline->apply_lane_batch(batch);
    emit_authored_lane_created(lane);
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

    auto const connections_before = bound_timeline->lane_connections();
    bound_timeline->connect_public_lanes_or_defer(
        request.source_lane_id,
        request.target_lane_id,
        LanePortId{
            .domain = request.port_domain,
            .kind = request.port_kind,
            .ordinal = request.port_ordinal,
        });
    if (request.authored && bound_authored_lanes != nullptr) {
        bound_authored_lanes->record_connection(AuthoredLaneConnection{
            .source_lane_id = request.source_lane_id,
            .target_lane_id = request.target_lane_id,
            .input = LanePortId{
                .domain = request.port_domain,
                .kind = request.port_kind,
                .ordinal = request.port_ordinal,
            },
        });
    }
    auto const connections_after = bound_timeline->lane_connections();
    auto changed_lanes = affected_lanes_from_connection_delta(connections_before, connections_after);
    if (!changed_lanes.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_timeline_lanes_changed_event,
            TimelineLanesChanged{
                .version_index = timeline_change_version_index++,
                .lane_set_changed = false,
                .metadata_for_lane = [timeline = bound_timeline](LaneId lane_id) {
                    return timeline->lane_metadata(lane_id);
                },
                .model_type_id_for_lane = [timeline = bound_timeline](LaneId lane_id) {
                    return timeline->lane_model_type_id(lane_id);
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
                .changed_lanes = std::move(changed_lanes),
            });
    }
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
    ProjectSetTimelineLaneUiStateRequestedEvent,
    iv_runtime_project_set_timeline_lane_ui_state_requested_event,
    handle_set_timeline_lane_ui_state);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectGetTimelineLaneTypesRequestedEvent,
    iv_runtime_project_get_timeline_lane_types_requested_event,
    handle_get_timeline_lane_types);
IV_SUBSCRIBE_LINKER_EVENT(
    ProjectCreateTimelineLaneRequestedEvent,
    iv_runtime_project_create_timeline_lane_requested_event,
    handle_create_timeline_lane);
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
            if (!bound_timeline->lane_is_persistent(lane)) {
                continue;
            }
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
        builder.add_authored_lane_connections(bound_authored_lanes == nullptr
            ? std::vector<AuthoredLaneConnection>{} : bound_authored_lanes->connections());
        builder.add_authored_lanes(bound_authored_lanes == nullptr
            ? std::vector<AuthoredLaneRecord>{} : bound_authored_lanes->records());
    });
} // namespace

void bind_runtime_project_timeline_execution_bridge(
    Timeline &timeline,
    TimelineExecution &timeline_execution,
    AuthoredLanes &authored_lanes,
    std::filesystem::path workspace_root)
{
    bound_timeline = &timeline;
    bound_timeline_execution = &timeline_execution;
    bound_authored_lanes = &authored_lanes;
    (void)workspace_root;
}

void bind_runtime_project_timeline_execution_bridge(
    Timeline &timeline,
    TimelineExecution &timeline_execution,
    std::filesystem::path workspace_root)
{
    bound_timeline = &timeline;
    bound_timeline_execution = &timeline_execution;
    bound_authored_lanes = nullptr;
    (void)workspace_root;
}

void unbind_runtime_project_timeline_execution_bridge(
    TimelineExecution const &timeline_execution)
{
    if (bound_timeline_execution == &timeline_execution) {
        bound_timeline = nullptr;
        bound_timeline_execution = nullptr;
        bound_authored_lanes = nullptr;
    }
}
} // namespace iv
