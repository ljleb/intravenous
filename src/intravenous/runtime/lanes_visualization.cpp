#include <intravenous/runtime/lanes_visualization.h>

#include <intravenous/runtime/lanes_visualization_events.h>
#include <intravenous/runtime/task_runner_events.h>

#include <algorithm>
#include <thread>
#include <variant>

namespace iv {
namespace {
enum class TrackedLaneKind {
    none,
    realtime_sample,
    realtime_event,
    compiled_sample,
    compiled_event,
};

TrackedLaneKind output_lane_kind(LaneOutputConfig const &output)
{
    if (std::holds_alternative<RealtimeSampleLaneOutputConfig>(output)) {
        return TrackedLaneKind::realtime_sample;
    }
    if (std::holds_alternative<RealtimeEventLaneOutputConfig>(output)) {
        return TrackedLaneKind::realtime_event;
    }
    if (std::holds_alternative<CompiledSampleLaneOutputConfig>(output)) {
        return TrackedLaneKind::compiled_sample;
    }
    return TrackedLaneKind::compiled_event;
}

void limit_events_to_display_columns(
    std::vector<TimedEvent> &events,
    size_t first_sample_index,
    size_t last_sample_index,
    size_t display_sample_count)
{
    if (display_sample_count == 0 || events.size() <= display_sample_count
        || last_sample_index < first_sample_index) {
        return;
    }

    // At most one marker can be seen in a horizontal display column. Keep the
    // first event in each column before serializing, rather than shipping (and
    // creating DOM for) potentially millions of indistinguishable events.
    auto const window_sample_count = last_sample_index - first_sample_index + 1;
    std::vector<TimedEvent> reduced;
    reduced.reserve(display_sample_count);
    std::optional<size_t> previous_column;
    for (auto const &event : events) {
        auto const offset = event.time > first_sample_index
            ? event.time - first_sample_index : 0;
        auto const column = std::min(
            display_sample_count - 1,
            (offset * display_sample_count) / window_sample_count);
        if (previous_column.has_value() && *previous_column == column) {
            continue;
        }
        reduced.push_back(event);
        previous_column = column;
    }
    events = std::move(reduced);
}
} // namespace

LanesVisualization::LanesVisualization(
    std::optional<std::chrono::milliseconds> publish_interval,
    size_t block_size) :
    block_size_(block_size == 0 ? 1 : block_size)
{
    if (!publish_interval.has_value()) {
        return;
    }
    publish_interval_ = *publish_interval;
    publisher_thread_.emplace([this](std::stop_token stop_token) {
        publisher_loop(stop_token);
    });
}

void LanesVisualization::handle_lane_views_updated(LaneViewResult const &update)
{
    auto const& new_lane_infos = update.lanes.lanes;
    auto tracked_kind_for = [this](LaneId lane) {
        if (tracked_sample_lanes_.contains(lane)) {
            return TrackedLaneKind::realtime_sample;
        }
        if (tracked_event_lanes_.contains(lane)) {
            return TrackedLaneKind::realtime_event;
        }
        return TrackedLaneKind::none;
    };

    // Determine which lanes need output-config classification because they are
    // new or their tracked realtime classification may have become stale.
    std::vector<LaneId> lanes_to_classify;
    {
        std::scoped_lock lock(mutex_);
        for (auto const &info : new_lane_infos) {
            auto const lane = info.runtime_lane;
            lanes_to_classify.push_back(lane);
        }
    }

    // Query output configs (no lock — fires linker events)
    std::unordered_map<uint64_t, LaneVisualizationOutputDescriptor> output_descriptors;
    for (auto const lane : lanes_to_classify) {
        LanesVisualizationLaneOutputQueryBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_lanes_visualization_lane_output_query_event,
            lane,
            builder);
        if (auto descriptor = builder.build()) {
            output_descriptors[lane.value] = std::move(*descriptor);
        }
    }

    // Allocate realtime level sinks; reduce compiled windows to one scalar.
    std::unordered_map<uint64_t, TrackedRealtimeSampleLane> new_sample_lanes;
    std::unordered_map<uint64_t, TrackedRealtimeEventLane> new_event_lanes;
    std::unordered_map<uint64_t, Sample::storage> compiled_sample_levels;
    std::unordered_map<uint64_t, std::vector<TimedEvent>> compiled_events;
    std::unordered_map<uint64_t, LaneUiStateSnapshot> initial_ui_states;

    for (auto const &info : new_lane_infos) {
        if (!info.model_type_id.has_value()) continue;
        LanesVisualizationLaneUiStateBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_lanes_visualization_lane_ui_state_query_event,
            info.runtime_lane,
            false,
            builder);
        if (auto snapshot = builder.build()) {
            initial_ui_states.emplace(info.runtime_lane.value, std::move(*snapshot));
        }
    }

    for (auto const lane : lanes_to_classify) {
        auto const it = output_descriptors.find(lane.value);
        if (it == output_descriptors.end()) {
            continue;
        }
        auto const &descriptor = it->second;
        auto const desired_kind = output_lane_kind(descriptor.config);
        if (desired_kind == TrackedLaneKind::realtime_sample) {
            auto const channel_layout =
                sample_channel_layout_for(descriptor.config, descriptor.sample_channel_type)
                    .value_or(ChannelLayout{
                        .channel_type = descriptor.sample_channel_type.value_or(ChannelTypeId::stereo),
                        .sample_layout = SampleStreamLayout::planar,
                    });
            auto vis_lane = lane_id_allocator_.next();
            auto queue = std::make_shared<RealtimeSampleBlockQueue>(channel_layout, block_size_);
            new_sample_lanes.emplace(lane.value, TrackedRealtimeSampleLane{
                .source_lane = lane,
                .vis_lane = vis_lane,
                .queue = std::move(queue),
                .sample_channel_type = descriptor.sample_channel_type.value_or(ChannelTypeId::stereo),
            });
        } else if (desired_kind == TrackedLaneKind::realtime_event) {
            auto vis_lane = lane_id_allocator_.next();
            auto queue = std::make_shared<RealtimeEventBlockQueue>();
            new_event_lanes.emplace(lane.value, TrackedRealtimeEventLane{
                .source_lane = lane,
                .vis_lane = vis_lane,
                .queue = std::move(queue),
            });
        } else if (desired_kind == TrackedLaneKind::compiled_sample) {
            if (update.display_sample_count > 0) {
                LanesVisualizationCompiledSampleLevelBuilder builder;
                IV_INVOKE_LINKER_EVENT(
                    iv_runtime_lanes_visualization_compiled_sample_level_requested_event,
                    lane,
                    update.first_sample_index,
                    update.last_sample_index,
                    builder);
                if (auto const level = builder.build()) {
                    compiled_sample_levels[lane.value] = *level;
                }
            }
        } else if (desired_kind == TrackedLaneKind::compiled_event) {
            LanesVisualizationCompiledEventWindowBuilder builder;
            IV_INVOKE_LINKER_EVENT(
                iv_runtime_lanes_visualization_compiled_event_window_requested_event,
                lane,
                update.first_sample_index,
                update.last_sample_index,
                builder);
            auto events = builder.build();
            limit_events_to_display_columns(
                events,
                update.first_sample_index,
                update.last_sample_index,
                update.display_sample_count);
            compiled_events[lane.value] = std::move(events);
        }
    }

    // Apply under lock
    {
        std::scoped_lock lock(mutex_);

        for (auto const &[lane_value, descriptor] : output_descriptors) {
            auto const lane = LaneId { lane_value };
            auto const current_kind = tracked_kind_for(lane);
            auto const desired_kind = output_lane_kind(descriptor.config);

            if (current_kind == TrackedLaneKind::realtime_sample
                && desired_kind != TrackedLaneKind::realtime_sample) {
                if (auto it = tracked_sample_lanes_.find(lane); it != tracked_sample_lanes_.end()) {
                    if (it->second.registered_in_timeline) {
                        pending_timeline_lane_removals_.push_back(it->second.vis_lane);
                        draining_sample_queues_.push_back(it->second.queue);
                    }
                    tracked_sample_lanes_.erase(it);
                }
            } else if (current_kind == TrackedLaneKind::realtime_event
                && desired_kind != TrackedLaneKind::realtime_event) {
                if (auto it = tracked_event_lanes_.find(lane); it != tracked_event_lanes_.end()) {
                    if (it->second.registered_in_timeline) {
                        pending_timeline_lane_removals_.push_back(it->second.vis_lane);
                        draining_event_queues_.push_back(it->second.queue);
                    }
                    tracked_event_lanes_.erase(it);
                }
            }
        }

        for (auto const &[lane_value, descriptor] : output_descriptors) {
            auto const lane = LaneId { lane_value };
            if (output_lane_kind(descriptor.config) != TrackedLaneKind::realtime_sample) {
                continue;
            }
            auto const tracked_it = tracked_sample_lanes_.find(lane);
            if (tracked_it == tracked_sample_lanes_.end()) {
                continue;
            }
            auto const desired_channel_type =
                descriptor.sample_channel_type.value_or(ChannelTypeId::stereo);
            if (tracked_it->second.sample_channel_type == desired_channel_type) {
                continue;
            }
            if (tracked_it->second.registered_in_timeline) {
                pending_timeline_lane_removals_.push_back(tracked_it->second.vis_lane);
                draining_sample_queues_.push_back(tracked_it->second.queue);
            }
            tracked_sample_lanes_.erase(tracked_it);
            auto const channel_layout =
                sample_channel_layout_for(descriptor.config, descriptor.sample_channel_type)
                    .value_or(ChannelLayout{
                        .channel_type = desired_channel_type,
                        .sample_layout = SampleStreamLayout::planar,
                    });
            auto queue = std::make_shared<RealtimeSampleBlockQueue>(channel_layout, block_size_);
            tracked_sample_lanes_.emplace(lane, TrackedRealtimeSampleLane{
                .source_lane = lane,
                .vis_lane = lane_id_allocator_.next(),
                .queue = std::move(queue),
                .sample_channel_type = desired_channel_type,
            });
        }

        for (auto &[lane_value, tracked] : new_sample_lanes) {
            auto const lane = LaneId { lane_value };
            if (!tracked_sample_lanes_.contains(lane)) {
                tracked_sample_lanes_.emplace(lane, std::move(tracked));
            }
        }
        for (auto &[lane_value, tracked] : new_event_lanes) {
            auto const lane = LaneId { lane_value };
            if (!tracked_event_lanes_.contains(lane)) {
                tracked_event_lanes_.emplace(lane, std::move(tracked));
            }
        }

        ActiveView view;
        view.view_id = update.view_id;
        view.first_sample_index = update.first_sample_index;
        view.last_sample_index = update.last_sample_index;
        view.display_sample_count = update.display_sample_count;

        for (auto const &info : new_lane_infos) {
            auto const lane = info.runtime_lane;
            if (info.model_type_id.has_value()) {
                view.ui_model_lanes.push_back(lane);
                if (auto const state = initial_ui_states.find(lane.value);
                    state != initial_ui_states.end()) {
                    view.ui_states.emplace(lane.value, state->second);
                    view.pending_ui_state_lanes.insert(lane.value);
                }
            }
            auto const cfg_it = output_descriptors.find(lane.value);
            auto const desired_kind =
                cfg_it != output_descriptors.end()
                    ? output_lane_kind(cfg_it->second.config)
                    : tracked_kind_for(lane);
            view.public_lane_ids_by_runtime_lane[lane.value] = info.lane_id;
            if (desired_kind == TrackedLaneKind::realtime_sample && tracked_sample_lanes_.contains(lane)) {
                view.realtime_sample_lanes.push_back(lane);
            } else if (desired_kind == TrackedLaneKind::realtime_event && tracked_event_lanes_.contains(lane)) {
                view.realtime_event_lanes.push_back(lane);
            } else {
                if (cfg_it == output_descriptors.end()) {
                    continue;
                }
                if (desired_kind == TrackedLaneKind::compiled_sample) {
                    view.compiled_sample_lanes.push_back(lane);
                    auto sit = compiled_sample_levels.find(lane.value);
                    if (sit != compiled_sample_levels.end()) {
                        view.compiled_sample_levels[lane.value] = sit->second;
                    }
                } else if (desired_kind == TrackedLaneKind::compiled_event) {
                    view.compiled_event_lanes.push_back(lane);
                    auto eit = compiled_events.find(lane.value);
                    if (eit != compiled_events.end()) {
                        view.compiled_event_data[lane.value] = std::move(eit->second);
                    }
                }
            }
        }

        active_views_[update.view_id] = std::move(view);
    }
}

void LanesVisualization::handle_lane_view_closed(InternedString view_id)
{
    std::scoped_lock lock(mutex_);
    active_views_.erase(view_id);
}

void LanesVisualization::handle_task_runner_after_pass(
    TasksRunnerAfterPass const &)
{
    // Compute which source lanes are currently in use by any active view
    std::unordered_set<uint64_t> used_sample_sources;
    std::unordered_set<uint64_t> used_event_sources;

    struct SampleAddInfo {
        LaneId source_lane {};
        LaneId vis_lane {};
        RealtimeSampleBlockQueue *queue_ptr = nullptr;
        ChannelTypeId sample_channel_type = ChannelTypeId::stereo;
    };
    struct EventAddInfo {
        LaneId source_lane {};
        LaneId vis_lane {};
        RealtimeEventBlockQueue *queue_ptr = nullptr;
    };

    std::vector<SampleAddInfo> sample_additions;
    std::vector<EventAddInfo> event_additions;
    TimelineLaneBatchUpdate batch;

    {
        std::scoped_lock lock(mutex_);
        batch.removals.insert(
            batch.removals.end(),
            pending_timeline_lane_removals_.begin(),
            pending_timeline_lane_removals_.end());
        pending_timeline_lane_removals_.clear();

        for (auto const &[_, view] : active_views_) {
            for (auto const lane : view.realtime_sample_lanes) {
                used_sample_sources.insert(lane.value);
            }
            for (auto const lane : view.realtime_event_lanes) {
                used_event_sources.insert(lane.value);
            }
        }

        // Process tracked sample lanes
        std::vector<LaneId> sample_to_erase;
        for (auto &[source_lane, tracked] : tracked_sample_lanes_) {
            bool const in_use = used_sample_sources.contains(source_lane.value);
            if (!tracked.registered_in_timeline && in_use) {
                sample_additions.push_back(SampleAddInfo{
                    .source_lane = source_lane,
                    .vis_lane = tracked.vis_lane,
                    .queue_ptr = tracked.queue.get(),
                    .sample_channel_type = tracked.sample_channel_type,
                });
                tracked.registered_in_timeline = true;
            } else if (tracked.registered_in_timeline && !in_use) {
                batch.removals.push_back(tracked.vis_lane);
                draining_sample_queues_.push_back(tracked.queue);
                sample_to_erase.push_back(source_lane);
            } else if (!tracked.registered_in_timeline && !in_use) {
                // Never added to Timeline, just cleanup
                draining_sample_queues_.push_back(tracked.queue);
                sample_to_erase.push_back(source_lane);
            }
        }
        for (auto const lane : sample_to_erase) {
            tracked_sample_lanes_.erase(lane);
        }

        // Process tracked event lanes
        std::vector<LaneId> event_to_erase;
        for (auto &[source_lane, tracked] : tracked_event_lanes_) {
            bool const in_use = used_event_sources.contains(source_lane.value);
            if (!tracked.registered_in_timeline && in_use) {
                event_additions.push_back(EventAddInfo{
                    .source_lane = source_lane,
                    .vis_lane = tracked.vis_lane,
                    .queue_ptr = tracked.queue.get(),
                });
                tracked.registered_in_timeline = true;
            } else if (tracked.registered_in_timeline && !in_use) {
                batch.removals.push_back(tracked.vis_lane);
                draining_event_queues_.push_back(tracked.queue);
                event_to_erase.push_back(source_lane);
            } else if (!tracked.registered_in_timeline && !in_use) {
                draining_event_queues_.push_back(tracked.queue);
                event_to_erase.push_back(source_lane);
            }
        }
        for (auto const lane : event_to_erase) {
            tracked_event_lanes_.erase(lane);
        }
    }

    // Build batch entries (outside lock — queue raw ptrs are safe: queues kept
    // alive by shared_ptr in tracked_sample_lanes_ which only grows via
    // handle_lane_views_updated; we marked additions as registered under lock)
    for (auto const &add : sample_additions) {
        auto *queue_ptr = add.queue_ptr;
        auto const vis_lane = add.vis_lane;
        auto const source_lane = add.source_lane;
        batch.upserts.push_back(TimelineLaneUpsert{
            .lane = vis_lane,
            .lifetime = TimelineLaneLifetime::ephemeral,
            .make_node = [queue_ptr] {
                return TypeErasedLaneNode(VisualizationRealtimeSampleLane{ .queue = queue_ptr });
            },
            .sample_channel_type = add.sample_channel_type,
        });
        batch.connections_to_add.push_back(LaneGraphConnection{
            .source = source_lane,
            .target = vis_lane,
            .input = realtime_sample_input(0),
        });
    }
    for (auto const &add : event_additions) {
        auto *queue_ptr = add.queue_ptr;
        auto const vis_lane = add.vis_lane;
        auto const source_lane = add.source_lane;
        batch.upserts.push_back(TimelineLaneUpsert{
            .lane = vis_lane,
            .lifetime = TimelineLaneLifetime::ephemeral,
            .make_node = [queue_ptr] {
                return TypeErasedLaneNode(VisualizationRealtimeEventLane{ .queue = queue_ptr });
            },
        });
        batch.connections_to_add.push_back(LaneGraphConnection{
            .source = source_lane,
            .target = vis_lane,
            .input = realtime_event_input(0),
        });
    }

    bool const batch_has_changes =
        !batch.upserts.empty() || !batch.removals.empty() || !batch.connections_to_add.empty();
    if (batch_has_changes) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_lanes_visualization_timeline_batch_requested_event,
            batch);
    }
}

void LanesVisualization::publish_now()
{
    LanesVisualizationPlaybackPositionBuilder playback_position_builder;
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_lanes_visualization_playback_position_query_event,
        playback_position_builder);
    auto const playback_sample_index = playback_position_builder.build();

    // UI model serialization is strictly demand-driven: the lane itself
    // decides whether its cheap dirty flag warrants a snapshot this frame.
    std::unordered_set<uint64_t> visible_ui_model_lanes;
    {
        std::scoped_lock lock(mutex_);
        for (auto const &[_, view] : active_views_) {
            for (auto const lane : view.ui_model_lanes) {
                visible_ui_model_lanes.insert(lane.value);
            }
        }
    }
    std::unordered_map<uint64_t, LaneUiStateSnapshot> changed_ui_states;
    for (auto const lane_value : visible_ui_model_lanes) {
        LanesVisualizationLaneUiStateBuilder builder;
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_lanes_visualization_lane_ui_state_query_event,
            LaneId{lane_value},
            true,
            builder);
        if (auto snapshot = builder.build()) {
            changed_ui_states.emplace(lane_value, std::move(*snapshot));
        }
    }

    // Snapshot queue shared_ptrs and draining queues under lock (brief)
    struct SampleSnapshot {
        LaneId source_lane {};
        std::shared_ptr<RealtimeSampleBlockQueue> queue {};
    };
    struct EventSnapshot {
        LaneId source_lane {};
        std::shared_ptr<RealtimeEventBlockQueue> queue {};
    };

    std::vector<SampleSnapshot> sample_snapshots;
    std::vector<EventSnapshot> event_snapshots;
    std::vector<std::shared_ptr<RealtimeEventBlockQueue>> draining_event;

    {
        std::scoped_lock lock(mutex_);
        for (auto const &[source_lane, tracked] : tracked_sample_lanes_) {
            sample_snapshots.push_back(SampleSnapshot{ source_lane, tracked.queue });
        }
        for (auto const &[source_lane, tracked] : tracked_event_lanes_) {
            event_snapshots.push_back(EventSnapshot{ source_lane, tracked.queue });
        }
        draining_sample_queues_.clear();
        draining_event = std::move(draining_event_queues_);
    }

    // Read one sampled level per realtime lane. No sample blocks cross this
    // boundary and no audio buffers are copied for visualization.
    std::unordered_map<uint64_t, Sample::storage> sampled_levels;
    std::unordered_map<uint64_t, std::vector<TimedEvent>> drained_events;

    for (auto &snap : sample_snapshots) {
        sampled_levels.emplace(snap.source_lane.value, snap.queue->peak_level());
    }
    for (auto &snap : event_snapshots) {
        auto &buf = drained_events[snap.source_lane.value];
        snap.queue->drain([&](std::span<TimedEvent const> events) {
            buf.insert(buf.end(), events.begin(), events.end());
        });
    }
    // Event queues retain their drain semantics because event detail is still
    // part of the visualization API. Sample sinks need no removal drain.
    for (auto &q : draining_event) {
        q->drain([](std::span<TimedEvent const>) {});
    }
    // shared_ptrs released here — frees queues no longer needed

    // Accumulate event history and build updates under lock.
    std::vector<LaneViewContentUpdate> updates;
    {
        std::scoped_lock lock(mutex_);
        for (auto &[lane_value, events] : drained_events) {
            auto const it = tracked_event_lanes_.find(LaneId { lane_value });
            if (it == tracked_event_lanes_.end()) {
                continue;
            }
            auto &hist = it->second.history;
            hist.insert(hist.end(), events.begin(), events.end());
        }
        for (auto &[_, view] : active_views_) {
            for (auto const &[lane_value, state] : changed_ui_states) {
                if (std::find(view.ui_model_lanes.begin(), view.ui_model_lanes.end(),
                              LaneId{lane_value}) == view.ui_model_lanes.end()) {
                    continue;
                }
                view.ui_states[lane_value] = state;
                view.pending_ui_state_lanes.insert(lane_value);
            }
        }

        updates.reserve(active_views_.size());
        for (auto &[view_id, view] : active_views_) {
            LaneViewContentUpdate content {
                .view_id = view_id,
                .playback_sample_index = playback_sample_index,
            };

            for (auto const lane_value : view.pending_ui_state_lanes) {
                auto const state = view.ui_states.find(lane_value);
                auto const public_id = view.public_lane_ids_by_runtime_lane.find(lane_value);
                if (state == view.ui_states.end()
                    || public_id == view.public_lane_ids_by_runtime_lane.end()) continue;
                content.ui_states.push_back(LaneUiStateUpdate{
                    .lane_id = public_id->second,
                    .revision = state->second.revision,
                    .serialized_state = state->second.serialized_state,
                });
            }
            view.pending_ui_state_lanes.clear();

            for (auto const lane : view.realtime_sample_lanes) {
                auto const it = tracked_sample_lanes_.find(lane);
                if (it == tracked_sample_lanes_.end()) {
                    continue;
                }
                auto const peak = sampled_levels.contains(lane.value)
                    ? sampled_levels.at(lane.value)
                    : Sample::storage { 0.0f };
                content.lanes.push_back(LaneVisualizationSeries{
                    .lane_id = view.public_lane_ids_by_runtime_lane.at(lane.value),
                    .adapter_type = "level",
                    .sample_channel_type = it->second.sample_channel_type,
                    .peak_level = peak,
                    .secondary_peak_level = it->second.sample_channel_type == ChannelTypeId::stereo
                        ? std::optional<Sample::storage>{it->second.queue->secondary_peak_level()}
                        : std::nullopt,
                });
            }

            for (auto const lane : view.realtime_event_lanes) {
                auto const it = tracked_event_lanes_.find(lane);
                if (it == tracked_event_lanes_.end()) {
                    continue;
                }
                auto const &hist = it->second.history;
                if (hist.empty()) {
                    continue;
                }
                content.lanes.push_back(LaneVisualizationSeries{
                    .lane_id = view.public_lane_ids_by_runtime_lane.at(lane.value),
                    .adapter_type = "activity",
                    .event_count = hist.size(),
                });
            }

            for (auto const lane : view.compiled_sample_lanes) {
                auto const dit = view.compiled_sample_levels.find(lane.value);
                if (dit == view.compiled_sample_levels.end()) {
                    continue;
                }
                content.lanes.push_back(LaneVisualizationSeries{
                    .lane_id = view.public_lane_ids_by_runtime_lane.at(lane.value),
                    .adapter_type = "level",
                    .peak_level = dit->second,
                });
            }

            for (auto const lane : view.compiled_event_lanes) {
                auto const dit = view.compiled_event_data.find(lane.value);
                if (dit == view.compiled_event_data.end() || dit->second.empty()) {
                    continue;
                }
                content.lanes.push_back(LaneVisualizationSeries{
                    .lane_id = view.public_lane_ids_by_runtime_lane.at(lane.value),
                    .adapter_type = "events",
                    .events = dit->second,
                });
            }

            updates.push_back(std::move(content));
        }
    }

    for (auto const &update : updates) {
        IV_INVOKE_LINKER_EVENT(iv_runtime_lane_view_content_updated_event, update);
    }
}

void LanesVisualization::publisher_loop(std::stop_token stop_token)
{
    while (!stop_token.stop_requested()) {
        std::this_thread::sleep_for(publish_interval_);
        if (stop_token.stop_requested()) {
            break;
        }
        publish_now();
    }
}
} // namespace iv
