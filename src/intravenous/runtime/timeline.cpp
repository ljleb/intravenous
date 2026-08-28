#include <intravenous/runtime/timeline.h>

#include <intravenous/basic_lane_nodes/controls.h>
#include <intravenous/runtime/authored_lanes_events.h>
#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/lanes_visualization_events.h>
#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/timeline_execution_events.h>

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <unordered_map>

namespace iv {
namespace {
class TimelineLaneQueryDatasetView final : public query::LaneQueryDataset {
    Timeline *timeline_;
    query::LaneQuerySchema schema_;
    std::vector<LaneId> lanes_;

public:
    TimelineLaneQueryDatasetView(
        Timeline &timeline,
        query::LaneQuerySchema schema,
        std::vector<LaneId> lanes)
        : timeline_(&timeline),
          schema_(std::move(schema)),
          lanes_(std::move(lanes))
    {}

    query::LaneQuerySchema const &schema() const override { return schema_; }
    size_t lane_count() const override { return lanes_.size(); }
    std::uint64_t lane_id_at(size_t index) const override { return lanes_.at(index).value; }
    bool in_filter(size_t, std::string_view) const override { return false; }

    bool has_unit(size_t index, query::LaneQueryPropertyId property) const override
    {
        return timeline_->lane_has_unit_metadata(lanes_.at(index), schema_.key_of(property));
    }

    std::optional<int> int_value(size_t index, query::LaneQueryPropertyId property) const override
    {
        return timeline_->lane_int_metadata(lanes_.at(index), schema_.key_of(property));
    }

    std::optional<float> float_value(size_t index, query::LaneQueryPropertyId property) const override
    {
        return timeline_->lane_float_metadata(lanes_.at(index), schema_.key_of(property));
    }
};

std::vector<TimelineLaneOutputs> outputs_for_lanes(
    Timeline &timeline,
    std::vector<LaneId> const &lanes)
{
    std::vector<TimelineLaneOutputs> outputs;
    outputs.reserve(lanes.size());
    for (auto const lane : lanes) {
        outputs.push_back(TimelineLaneOutputs{
            .lane = lane,
            .outputs = timeline.lane_outputs_for(lane),
        });
    }
    return outputs;
}

std::vector<LaneId> changed_lanes_from_connection_delta(
    std::vector<LaneGraphConnection> const &before,
    std::vector<LaneGraphConnection> const &after)
{
    auto contains = [](std::vector<LaneGraphConnection> const &connections,
                       LaneGraphConnection const &candidate) {
        return std::ranges::find(connections, candidate) != connections.end();
    };

    std::unordered_set<std::uint64_t> values;
    for (auto const &connection : after) {
        if (!contains(before, connection)) {
            values.insert(connection.source.value);
            values.insert(connection.target.value);
        }
    }
    for (auto const &connection : before) {
        if (!contains(after, connection)) {
            values.insert(connection.source.value);
            values.insert(connection.target.value);
        }
    }

    std::vector<LaneId> lanes;
    lanes.reserve(values.size());
    for (auto const value : values) {
        lanes.push_back(LaneId{value});
    }
    return lanes;
}

std::vector<LaneId> execution_affected_lanes_from_connection_delta(
    std::vector<LaneGraphConnection> const &before,
    std::vector<LaneGraphConnection> const &after)
{
    auto contains = [](std::vector<LaneGraphConnection> const &connections,
                       LaneGraphConnection const &candidate) {
        return std::ranges::find(connections, candidate) != connections.end();
    };

    std::unordered_map<std::uint64_t, std::vector<LaneId>> targets_by_source;
    for (auto const &connection : after) {
        targets_by_source[connection.source.value].push_back(connection.target);
    }

    std::vector<LaneId> affected;
    std::unordered_set<std::uint64_t> visited;
    auto include_target_and_descendants = [&](LaneId initial_target) {
        std::vector<LaneId> pending{initial_target};
        while (!pending.empty()) {
            auto const lane = pending.back();
            pending.pop_back();
            if (!visited.insert(lane.value).second) {
                continue;
            }
            affected.push_back(lane);
            if (auto const it = targets_by_source.find(lane.value);
                it != targets_by_source.end()) {
                pending.insert(pending.end(), it->second.begin(), it->second.end());
            }
        }
    };

    for (auto const &connection : after) {
        if (!contains(before, connection)) {
            include_target_and_descendants(connection.target);
        }
    }
    for (auto const &connection : before) {
        if (!contains(after, connection)) {
            include_target_and_descendants(connection.target);
        }
    }
    return affected;
}

void emit_lane_topology_diagnostic(std::string message)
{
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_notification_event,
        ProjectNotification(ProjectMessageNotification{
            .level = "debug",
            .message = "lane topology diagnostic: " + std::move(message),
        }));
}

bool has_connection(
    std::vector<LaneGraphConnection> const &connections,
    LaneGraphConnection const &connection)
{
    return std::ranges::find(connections, connection) != connections.end();
}

void normalize_lane_delta(
    std::vector<LaneId> &created_lanes,
    std::vector<LaneId> &removed_lanes,
    std::vector<LaneId> &changed_lanes)
{
    auto deduplicate = [](std::vector<LaneId> &lanes) {
        std::unordered_set<std::uint64_t> seen;
        std::erase_if(lanes, [&seen](LaneId lane) {
            return !seen.insert(lane.value).second;
        });
    };

    deduplicate(created_lanes);
    deduplicate(removed_lanes);
    deduplicate(changed_lanes);

    std::unordered_set<std::uint64_t> blocked;
    for (auto const lane : created_lanes) {
        blocked.insert(lane.value);
    }
    for (auto const lane : removed_lanes) {
        blocked.insert(lane.value);
    }
    std::erase_if(changed_lanes, [&blocked](LaneId lane) {
        return blocked.contains(lane.value);
    });
}
} // namespace

void Timeline::publish_lane_batch_change(
    TimelineLaneBatchUpdate const &batch,
    std::vector<LaneId> changed_lanes)
{
    std::vector<LaneId> created_lanes;
    created_lanes.reserve(batch.upserts.size());
    for (auto const &upsert : batch.upserts) {
        created_lanes.push_back(upsert.lane);
    }
    auto removed_lanes = batch.removals;
    normalize_lane_delta(created_lanes, removed_lanes, changed_lanes);

    auto [schema, schema_change] = reconcile_lane_query_schema();
    TimelineLanesChanged change{
        .version_index = batch.version_index,
        .lane_set_changed = true,
        .dataset = std::make_shared<TimelineLaneQueryDatasetView>(
            *this,
            schema,
            persistent_lane_ids()),
        .schema_change = std::move(schema_change),
        .schema = std::move(schema),
        .metadata_for_lane = [this](LaneId lane) {
            return lane_metadata(lane);
        },
        .model_type_id_for_lane = [this](LaneId lane) {
            return lane_model_type_id(lane);
        },
        .public_id_for_lane = [this](LaneId lane) {
            return lane_public_id(lane);
        },
        .outputs_for_lanes = [this](std::vector<LaneId> const &lanes) {
            return outputs_for_lanes(*this, lanes);
        },
        .visit_lanes = [this](std::vector<LaneId> const &lanes,
                              TimelineLaneVisitFn const &visit) {
            with_graph([&](LaneGraph const &graph) {
                for (auto const lane : lanes) {
                    if (!graph.contains(lane)) {
                        continue;
                    }
                    auto const &record = graph.lane(lane);
                    visit(
                        lane,
                        record.node,
                        record.output,
                        record.sample_channel_type,
                        graph.inputs_for(lane),
                        record.external_task_dependencies);
                }
            });
        },
        .created_lanes = std::move(created_lanes),
        .removed_lanes = std::move(removed_lanes),
        .changed_lanes = std::move(changed_lanes),
    };
    IV_INVOKE_LINKER_EVENT(iv_runtime_timeline_lanes_changed_event, change);
}

void Timeline::publish_project_lane_change(std::vector<LaneId> changed_lanes)
{
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_timeline_lanes_changed_event,
        TimelineLanesChanged{
            .version_index = _project_change_version_index++,
            .lane_set_changed = false,
            .metadata_for_lane = [this](LaneId lane) {
                return lane_metadata(lane);
            },
            .model_type_id_for_lane = [this](LaneId lane) {
                return lane_model_type_id(lane);
            },
            .public_id_for_lane = [this](LaneId lane) {
                return lane_public_id(lane);
            },
            .outputs_for_lanes = [this](std::vector<LaneId> const &lanes) {
                return outputs_for_lanes(*this, lanes);
            },
            .visit_lanes = [this](std::vector<LaneId> const &lanes,
                                  TimelineLaneVisitFn const &visit) {
                with_graph([&](LaneGraph const &graph) {
                    for (auto const lane : lanes) {
                        if (!graph.contains(lane)) {
                            continue;
                        }
                        auto const &record = graph.lane(lane);
                        visit(
                            lane,
                            record.node,
                            record.output,
                            record.sample_channel_type,
                            graph.inputs_for(lane),
                            record.external_task_dependencies);
                    }
                });
            },
            .changed_lanes = std::move(changed_lanes),
        });
}

void Timeline::apply_lane_batch_and_publish_change(TimelineLaneBatchUpdate const &batch)
{
    auto const connections_before = lane_connections();
    apply_lane_batch(batch);
    publish_lane_batch_change(
        batch,
        changed_lanes_from_connection_delta(connections_before, lane_connections()));
}

void Timeline::handle_audio_device_lanes_timeline_batch(
    TimelineLaneBatchUpdate const &batch)
{
    apply_lane_batch_and_publish_change(batch);
}

void Timeline::handle_graph_input_lanes_timeline_batch(
    TimelineLaneBatchUpdate const &batch,
    GraphInputLanesAckBuilder &builder)
{
    apply_lane_batch_and_publish_change(batch);
    builder.succeed();
}

void Timeline::handle_lanes_visualization_timeline_batch(
    TimelineLaneBatchUpdate const &batch)
{
    apply_lane_batch_and_publish_change(batch);
}

void Timeline::handle_authored_lanes_timeline_batch(
    TimelineLaneBatchUpdate const &batch)
{
    apply_lane_batch_and_publish_change(batch);
}

void Timeline::handle_project_set_timeline_lane_sample_channel_type(
    ProjectSetTimelineLaneSampleChannelTypeRequest const &request,
    ProjectAckBuilder &builder)
{
    auto const lane = resolve_public_lane_id(request.lane_id);
    if (!lane.has_value()) {
        throw std::runtime_error("timeline lane not found");
    }
    with_graph([&](LaneGraph &graph) {
        auto &record = graph.lane(*lane);
        if (!record.sample_channel_type.has_value()) {
            throw std::runtime_error("timeline lane does not produce samples");
        }
        record.sample_channel_type = request.sample_channel_type;
    });
    publish_project_lane_change({*lane});
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void Timeline::handle_project_set_timeline_lane_ui_state(
    ProjectSetTimelineLaneUiStateRequest const &request,
    ProjectAckBuilder &builder)
{
    auto const lane = resolve_public_lane_id(request.lane_id);
    if (!lane.has_value()) {
        throw std::runtime_error("timeline lane not found");
    }

    LaneUiStateApplyResult result {};
    if (request.serialized_state.has_value()) {
        result = apply_lane_ui_state(*lane, LaneUiStateWrite{
            .expected_revision = request.expected_revision,
            .serialized_state = *request.serialized_state,
        });
        if (!result.accepted) {
            throw std::runtime_error(result.error_message.empty()
                ? "timeline lane rejected UI state" : result.error_message);
        }
        auto const snapshot = lane_ui_state_snapshot(*lane);
        if (!snapshot.has_value()) {
            throw std::runtime_error("authored lane did not provide canonical UI state");
        }
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_timeline_authored_lane_canonical_state_updated_event,
            request.lane_id,
            snapshot->serialized_state);
    }
    if (request.name.has_value()) {
        with_graph([&](LaneGraph &graph) {
            graph.lane(*lane).metadata.set_string("lane.name", *request.name);
        });
    }

    if (request.name.has_value() || result.effect != LaneUiStateEffect::ui_only) {
        // The view refresh is driven by the lane-change event.  Invalidate
        // first so it cannot query an old compiled window.
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_timeline_execution_invalidate_compiled_cache_event,
            *lane);
        publish_project_lane_change({*lane});
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void Timeline::handle_project_connect_timeline_lanes(
    ProjectConnectTimelineLanesRequest const &request,
    ProjectAckBuilder &builder)
{
    auto const source = resolve_public_lane_id(request.source_lane_id);
    auto const target = resolve_public_lane_id(request.target_lane_id);
    LanePortId const input{
        .domain = request.port_domain,
        .kind = request.port_kind,
        .ordinal = request.port_ordinal,
    };
    auto const connections_before = lane_connections();
    connect_public_lanes_or_defer(request.source_lane_id, request.target_lane_id, input);
    if (request.authored) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_timeline_authored_lane_connection_recorded_event,
            AuthoredLaneConnection{
                .source_lane_id = request.source_lane_id,
                .target_lane_id = request.target_lane_id,
                .input = input,
            });
    }
    auto const connections_after = lane_connections();
    if (source.has_value() && target.has_value()) {
        LaneGraphConnection const connection{
            .source = *source,
            .target = *target,
            .input = input,
        };
        emit_lane_topology_diagnostic(
            "connect " + request.source_lane_id.str() + " -> "
            + request.target_lane_id.str() + " before="
            + (has_connection(connections_before, connection) ? "present" : "absent")
            + " after="
            + (has_connection(connections_after, connection) ? "present" : "absent"));
    } else {
        emit_lane_topology_diagnostic(
            "connect deferred because one or both lanes are not realized");
    }
    auto changed_lanes = execution_affected_lanes_from_connection_delta(
        connections_before,
        connections_after);
    if (!changed_lanes.empty()) {
        publish_project_lane_change(std::move(changed_lanes));
        emit_lane_topology_diagnostic("connect execution refresh emitted");
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void Timeline::handle_project_disconnect_timeline_lanes(
    ProjectDisconnectTimelineLanesRequest const &request,
    ProjectAckBuilder &builder)
{
    auto const source = resolve_public_lane_id(request.source_lane_id);
    auto const target = resolve_public_lane_id(request.target_lane_id);
    if (!source.has_value() || !target.has_value()) {
        throw std::runtime_error("timeline lane not found");
    }
    LanePortId const input{
        .domain = request.port_domain,
        .kind = request.port_kind,
        .ordinal = request.port_ordinal,
    };
    auto const connections_before = lane_connections();
    with_graph([&](LaneGraph &graph) {
        graph.disconnect(*source, *target, input);
    });
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_timeline_authored_lane_connection_removed_event,
        AuthoredLaneConnection{
            .source_lane_id = request.source_lane_id,
            .target_lane_id = request.target_lane_id,
            .input = input,
        });
    auto const connections_after = lane_connections();
    LaneGraphConnection const connection{
        .source = *source,
        .target = *target,
        .input = input,
    };
    emit_lane_topology_diagnostic(
        "disconnect " + request.source_lane_id.str() + " -> "
        + request.target_lane_id.str() + " before="
        + (has_connection(connections_before, connection) ? "present" : "absent")
        + " after="
        + (has_connection(connections_after, connection) ? "present" : "absent"));
    auto changed_lanes = execution_affected_lanes_from_connection_delta(
        connections_before,
        connections_after);
    if (!changed_lanes.empty()) {
        publish_project_lane_change(std::move(changed_lanes));
        emit_lane_topology_diagnostic("disconnect execution refresh emitted");
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void Timeline::handle_project_persistence_collect_state(
    ProjectPersistenceBuilder &builder)
{
    std::vector<ProjectSetTimelineLaneSampleChannelTypeRequest> lane_sample_channel_types;
    for (auto const lane : lane_ids()) {
        if (!lane_is_persistent(lane)) {
            continue;
        }
        auto const channel_type = lane_sample_channel_type(lane);
        if (!channel_type.has_value()) {
            continue;
        }
        lane_sample_channel_types.push_back(ProjectSetTimelineLaneSampleChannelTypeRequest{
            .lane_id = lane_public_id(lane),
            .sample_channel_type = *channel_type,
        });
    }
    builder.add_lane_sample_channel_types(std::move(lane_sample_channel_types));

    TimelineAuthoredLaneConnectionsBuilder authored_connections_builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_timeline_authored_lane_connections_requested_event,
        authored_connections_builder);
    auto const authored_connections = authored_connections_builder.has_response()
        ? authored_connections_builder.build()
        : std::vector<AuthoredLaneConnection>{};
    auto is_authored_connection = [&](AuthoredLaneConnection const &connection) {
        return std::ranges::any_of(authored_connections, [&](auto const &authored) {
            return authored.source_lane_id == connection.source_lane_id
                && authored.target_lane_id == connection.target_lane_id
                && authored.input == connection.input;
        });
    };

    std::vector<ProjectConnectTimelineLanesRequest> lane_connections;
    auto add_connection = [&](ProjectConnectTimelineLanesRequest connection) {
        auto const duplicate = std::ranges::any_of(lane_connections, [&](auto const &existing) {
            return existing.source_lane_id == connection.source_lane_id
                && existing.target_lane_id == connection.target_lane_id
                && existing.port_domain == connection.port_domain
                && existing.port_kind == connection.port_kind
                && existing.port_ordinal == connection.port_ordinal;
        });
        if (!duplicate) {
            lane_connections.push_back(std::move(connection));
        }
    };
    for (auto const &connection : this->lane_connections()) {
        if (!lane_is_persistent(connection.source)
            || !lane_is_persistent(connection.target)) {
            continue;
        }
        auto const authored_connection = AuthoredLaneConnection{
            .source_lane_id = lane_public_id(connection.source),
            .target_lane_id = lane_public_id(connection.target),
            .input = connection.input,
        };
        if (is_authored_connection(authored_connection)) {
            continue;
        }
        add_connection(ProjectConnectTimelineLanesRequest{
            .source_lane_id = authored_connection.source_lane_id,
            .target_lane_id = authored_connection.target_lane_id,
            .port_domain = authored_connection.input.domain,
            .port_kind = authored_connection.input.kind,
            .port_ordinal = authored_connection.input.ordinal,
        });
    }
    for (auto const &[source_lane_id, target_lane_id, input] : pending_public_connections()) {
        add_connection(ProjectConnectTimelineLanesRequest{
            .source_lane_id = source_lane_id,
            .target_lane_id = target_lane_id,
            .port_domain = input.domain,
            .port_kind = input.kind,
            .port_ordinal = input.ordinal,
        });
    }
    builder.add_lane_connections(std::move(lane_connections));
}

void Timeline::handle_graph_input_lanes_knob_value_updated(
    LaneId lane,
    Sample value)
{
    with_graph([&](LaneGraph &graph) {
        if (!graph.contains(lane)) {
            return;
        }
        if (auto *knob = graph.lane(lane).node.try_as<KnobLaneNode>()) {
            knob->value = value;
        } else if (auto *input = graph.lane(lane).node.try_as<GraphSampleInputLaneNode>()) {
            input->default_value = value;
        }
    });
}

void Timeline::handle_lanes_visualization_lane_output_query(
    LaneId lane,
    LanesVisualizationLaneOutputQueryBuilder &builder)
{
    std::optional<LaneVisualizationOutputDescriptor> descriptor;
    with_graph([&](LaneGraph const &graph) {
        if (!graph.contains(lane)) {
            return;
        }
        auto const &record = graph.lane(lane);
        descriptor = LaneVisualizationOutputDescriptor{
            .config = record.output,
            .sample_channel_type = record.sample_channel_type,
            .subscribes_to_compiled_output_changes =
                record.node.subscribes_to_compiled_output_changes(),
        };
    });
    if (descriptor.has_value()) {
        builder.succeed(std::move(*descriptor));
    }
}

void Timeline::handle_lanes_visualization_lane_ui_state_query(
    LaneId lane,
    bool changed_only,
    LanesVisualizationLaneUiStateBuilder &builder)
{
    auto snapshot = changed_only
        ? take_lane_ui_state_update(lane)
        : lane_ui_state_snapshot(lane);
    if (snapshot.has_value()) {
        builder.succeed(std::move(*snapshot));
    }
}
} // namespace iv
