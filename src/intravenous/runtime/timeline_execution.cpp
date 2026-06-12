#include <intravenous/runtime/timeline_execution.h>

#include <algorithm>
#include <queue>
#include <stdexcept>

namespace iv {
namespace {
bool is_realtime_sample_output(LaneOutputConfig const &output)
{
    return std::holds_alternative<RealtimeSampleLaneOutputConfig>(output);
}

bool is_realtime_event_output(LaneOutputConfig const &output)
{
    return std::holds_alternative<RealtimeEventLaneOutputConfig>(output);
}

bool is_compiled_sample_output(LaneOutputConfig const &output)
{
    return std::holds_alternative<CompiledSampleLaneOutputConfig>(output);
}

bool is_compiled_event_output(LaneOutputConfig const &output)
{
    return std::holds_alternative<CompiledEventLaneOutputConfig>(output);
}
}

TimelineExecution::TimelineExecution(size_t block_size) :
    block_size_(block_size)
{}

void TimelineExecution::invoke_lane_task(void *context)
{
    auto *callback = static_cast<LaneCallbackContext *>(context);
    if (!callback || !callback->execution) {
        return;
    }
    callback->execution->execute_lane_task(callback->lane);
}

TaskGraphUpdate TimelineExecution::synchronize_from_graph(LaneGraph const &graph)
{
    std::vector<TrackedLane> lanes;
    graph.for_each_lane([&](LaneRecord const &record) {
        lanes.push_back(TrackedLane {
            .id = record.id,
            .node = &record.node,
            .output = record.output,
            .inputs = graph.inputs_for(record.id),
        });
    });

    std::scoped_lock lock(mutex_);
    return replace_all_lanes_locked(std::move(lanes));
}

TaskGraphUpdate TimelineExecution::handle_timeline_lanes_changed(TimelineLanesChanged const &change)
{
    std::scoped_lock lock(mutex_);
    TaskGraphUpdate update;

    for (auto const lane : change.removed_lanes) {
        if (!tracked_lanes_.contains(lane)) {
            continue;
        }
        tracked_lanes_.erase(lane);
        callback_contexts_.erase(lane);
        realtime_event_blocks_.erase(lane);
        compiled_sample_cache_.erase(lane);
        compiled_event_cache_.erase(lane);
        update.to_delete.push_back(timeline_lane_task_id(lane));
    }

    if (change.visit_lanes) {
        std::vector<LaneId> visited = change.created_lanes;
        visited.insert(visited.end(), change.changed_lanes.begin(), change.changed_lanes.end());
        change.visit_lanes(visited, [&](LaneId lane, TypeErasedLaneNode const &node, LaneOutputConfig const &output, std::vector<LaneInputConnection> const &inputs) {
            auto &tracked = tracked_lanes_[lane];
            bool const existed = tracked.id == lane && tracked.node != nullptr;
            tracked.id = lane;
            tracked.node = &node;
            tracked.output = output;
            tracked.inputs = inputs;

            auto &callback = callback_contexts_[lane];
            if (!callback) {
                callback = std::make_unique<LaneCallbackContext>();
            }
            callback->execution = this;
            callback->lane = lane;
            tracked.callback = TaskCallback {
                .invoke = &TimelineExecution::invoke_lane_task,
                .context = callback.get(),
            };

            compiled_sample_cache_.erase(lane);
            compiled_event_cache_.erase(lane);

            if (existed) {
                update.to_update.push_back(TaskUpdateRecord {
                    .id = timeline_lane_task_id(lane),
                    .depends_on = [&] {
                        std::vector<std::string> depends_on;
                        depends_on.reserve(inputs.size());
                        for (auto const &input : inputs) {
                            depends_on.push_back(timeline_lane_task_id(input.source));
                        }
                        return std::optional<std::vector<std::string>>(std::move(depends_on));
                    }(),
                    .callback = tracked.callback,
                });
            } else {
                update.to_create.push_back(TaskRecord {
                    .id = timeline_lane_task_id(lane),
                    .depends_on = [&] {
                        std::vector<std::string> depends_on;
                        depends_on.reserve(inputs.size());
                        for (auto const &input : inputs) {
                            depends_on.push_back(timeline_lane_task_id(input.source));
                        }
                        return depends_on;
                    }(),
                    .callback = tracked.callback,
                });
            }
        });
    }

    rebuild_runtime_storage_locked();
    return update;
}

std::vector<LaneId> TimelineExecution::realtime_sample_output_lanes() const
{
    std::scoped_lock lock(mutex_);
    std::vector<LaneId> lanes;
    for (auto const &[lane, tracked] : tracked_lanes_) {
        if (is_realtime_sample_output(tracked.output)) {
            lanes.push_back(lane);
        }
    }
    return lanes;
}

void TimelineExecution::set_realtime_start_index(size_t start_index)
{
    std::scoped_lock lock(mutex_);
    current_start_index_ = start_index;
}

std::span<Sample const> TimelineExecution::realtime_sample_block(LaneId lane) const
{
    std::scoped_lock lock(mutex_);
    return realtime_sample_block_locked(lane);
}

std::span<TimedEvent const> TimelineExecution::realtime_event_block(LaneId lane) const
{
    std::scoped_lock lock(mutex_);
    auto const it = realtime_event_blocks_.find(lane);
    if (it == realtime_event_blocks_.end()) {
        return {};
    }
    return it->second;
}

std::vector<Sample> TimelineExecution::compiled_sample_block(LaneId lane, size_t start_index)
{
    std::scoped_lock lock(mutex_);
    return ensure_compiled_sample_block_locked(lane, start_index);
}

std::vector<TimedEvent> TimelineExecution::compiled_event_block(LaneId lane, size_t start_index)
{
    std::scoped_lock lock(mutex_);
    return ensure_compiled_event_block_locked(lane, start_index);
}

void TimelineExecution::execute_lane_task(LaneId lane)
{
    std::scoped_lock lock(mutex_);
    if (!tracked_lanes_.contains(lane)) {
        return;
    }
    execute_lane_locked(lane, current_start_index_);
}

void TimelineExecution::rebuild_runtime_storage_locked()
{
    realtime_sample_descriptors_.clear();
    realtime_sample_slot_by_lane_.clear();
    auto const order = topological_order_locked();
    for (auto const lane : order) {
        auto const &tracked = tracked_lanes_.at(lane);
        if (!is_realtime_sample_output(tracked.output)) {
            continue;
        }
        realtime_sample_slot_by_lane_[lane] = realtime_sample_descriptors_.size();
        realtime_sample_descriptors_.push_back(RealtimeSamplePortDescriptor {
            .lane = lane,
        });
    }
    realtime_sample_storage_.assign(
        realtime_sample_descriptors_.size() * block_size_,
        0.0f);

    for (auto const &[lane, tracked] : tracked_lanes_) {
        if (is_realtime_event_output(tracked.output)) {
            realtime_event_blocks_[lane].clear();
        } else {
            realtime_event_blocks_.erase(lane);
        }
    }
}

TaskGraphUpdate TimelineExecution::replace_all_lanes_locked(std::vector<TrackedLane> lanes)
{
    TaskGraphUpdate update;
    for (auto const &[lane, _] : tracked_lanes_) {
        update.to_delete.push_back(timeline_lane_task_id(lane));
    }

    tracked_lanes_.clear();
    callback_contexts_.clear();
    realtime_sample_descriptors_.clear();
    realtime_sample_slot_by_lane_.clear();
    realtime_sample_storage_.clear();
    realtime_event_blocks_.clear();
    compiled_sample_cache_.clear();
    compiled_event_cache_.clear();

    for (auto &tracked : lanes) {
        auto &stored = tracked_lanes_[tracked.id];
        stored = tracked;
        auto &callback = callback_contexts_[tracked.id];
        if (!callback) {
            callback = std::make_unique<LaneCallbackContext>();
        }
        callback->execution = this;
        callback->lane = tracked.id;
        stored.callback = TaskCallback {
            .invoke = &TimelineExecution::invoke_lane_task,
            .context = callback.get(),
        };

        std::vector<std::string> depends_on;
        depends_on.reserve(stored.inputs.size());
        for (auto const &input : stored.inputs) {
            depends_on.push_back(timeline_lane_task_id(input.source));
        }
        update.to_create.push_back(TaskRecord {
            .id = timeline_lane_task_id(stored.id),
            .depends_on = std::move(depends_on),
            .callback = stored.callback,
        });
    }

    rebuild_runtime_storage_locked();
    return update;
}

std::vector<LaneId> TimelineExecution::topological_order_locked() const
{
    std::unordered_map<LaneId, size_t, LaneIdHash> indegree;
    std::unordered_map<LaneId, std::vector<LaneId>, LaneIdHash> users;
    for (auto const &[lane, tracked] : tracked_lanes_) {
        indegree[lane] = 0;
    }
    for (auto const &[lane, tracked] : tracked_lanes_) {
        for (auto const &input : tracked.inputs) {
            if (!tracked_lanes_.contains(input.source)) {
                continue;
            }
            ++indegree[lane];
            users[input.source].push_back(lane);
        }
    }

    std::queue<LaneId> ready;
    for (auto const &[lane, degree] : indegree) {
        if (degree == 0) {
            ready.push(lane);
        }
    }

    std::vector<LaneId> order;
    while (!ready.empty()) {
        auto const lane = ready.front();
        ready.pop();
        order.push_back(lane);
        for (auto const user : users[lane]) {
            auto &degree = indegree[user];
            if (--degree == 0) {
                ready.push(user);
            }
        }
    }
    return order;
}

void TimelineExecution::execute_lane_locked(LaneId lane, size_t start_index)
{
    auto it = tracked_lanes_.find(lane);
    if (it == tracked_lanes_.end() || it->second.node == nullptr) {
        return;
    }
    auto &tracked = it->second;

    if (is_compiled_sample_output(tracked.output)) {
        auto const cache_it = compiled_sample_cache_.find(lane);
        if (cache_it != compiled_sample_cache_.end() && cache_it->second.valid && cache_it->second.start_index == start_index) {
            return;
        }
    } else if (is_compiled_event_output(tracked.output)) {
        auto const cache_it = compiled_event_cache_.find(lane);
        if (cache_it != compiled_event_cache_.end() && cache_it->second.valid && cache_it->second.start_index == start_index) {
            return;
        }
    }

    std::vector<CompiledSampleLaneInput> compiled_sample_inputs;
    std::vector<CompiledEventLaneInput> compiled_event_inputs;
    std::vector<RealtimeSampleLaneInput> realtime_sample_inputs;
    std::vector<RealtimeEventLaneInput> realtime_event_inputs;

    compiled_sample_inputs.resize(tracked.node->compiled_sample_inputs().size());
    compiled_event_inputs.resize(tracked.node->compiled_event_inputs().size());
    realtime_sample_inputs.resize(tracked.node->realtime_sample_inputs().size());
    realtime_event_inputs.resize(tracked.node->realtime_event_inputs().size());

    for (auto const &connection : tracked.inputs) {
        if (!tracked_lanes_.contains(connection.source)) {
            continue;
        }
        auto const &source = tracked_lanes_.at(connection.source);
        if (connection.input.kind == PortKind::sample) {
            if (connection.input.domain == LanePortDomain::compiled) {
                auto const &values = ensure_compiled_sample_block_locked(connection.source, start_index);
                compiled_sample_inputs[connection.input.ordinal].sources.push_back(std::span<Sample const>(values));
            } else if (is_compiled_sample_output(source.output)) {
                auto const &values = ensure_compiled_sample_block_locked(connection.source, start_index);
                realtime_sample_inputs[connection.input.ordinal].block_override = std::span<Sample const>(values);
                realtime_sample_inputs[connection.input.ordinal].active_start_index = start_index;
                realtime_sample_inputs[connection.input.ordinal].active_count = block_size_;
                realtime_sample_inputs[connection.input.ordinal].has_source = true;
            } else {
                auto const values = realtime_sample_block_locked(connection.source);
                realtime_sample_inputs[connection.input.ordinal].block_override = values;
                realtime_sample_inputs[connection.input.ordinal].active_start_index = start_index;
                realtime_sample_inputs[connection.input.ordinal].active_count = block_size_;
                realtime_sample_inputs[connection.input.ordinal].has_source = true;
            }
        } else {
            if (connection.input.domain == LanePortDomain::compiled) {
                auto const &events = ensure_compiled_event_block_locked(connection.source, start_index);
                compiled_event_inputs[connection.input.ordinal].sources.push_back(std::span<TimedEvent const>(events));
            } else if (is_compiled_event_output(source.output)) {
                auto const &events = ensure_compiled_event_block_locked(connection.source, start_index);
                realtime_event_inputs[connection.input.ordinal].block_override = std::span<TimedEvent const>(events);
                realtime_event_inputs[connection.input.ordinal].active_start_index = start_index;
                realtime_event_inputs[connection.input.ordinal].active_count = block_size_;
            } else {
                auto const &events = realtime_event_blocks_[connection.source];
                realtime_event_inputs[connection.input.ordinal].block_override = events;
                realtime_event_inputs[connection.input.ordinal].active_start_index = start_index;
                realtime_event_inputs[connection.input.ordinal].active_count = block_size_;
            }
        }
    }

    LaneOutputView output;
    if (is_realtime_sample_output(tracked.output)) {
        auto samples = realtime_sample_block_mutable_locked(lane);
        std::ranges::fill(samples, 0.0f);
        output = RealtimeSampleLaneOutput(BlockView<Sample> {
            .first = samples,
        });
    } else if (is_realtime_event_output(tracked.output)) {
        auto &events = realtime_event_blocks_[lane];
        events.clear();
        output = RealtimeEventLaneOutput(events);
    } else if (is_compiled_sample_output(tracked.output)) {
        auto &cache = compiled_sample_cache_[lane];
        cache.start_index = start_index;
        cache.samples.assign(block_size_, 0.0f);
        cache.valid = true;
        output = CompiledSampleLaneOutput {
            .samples = &cache.samples,
            .window_start_index = start_index,
            .clamp_to_window = true,
        };
    } else {
        auto &cache = compiled_event_cache_[lane];
        cache.start_index = start_index;
        cache.events.clear();
        cache.valid = true;
        output = CompiledEventLaneOutput {
            .events = &cache.events,
        };
    }

    UntypedRealtimeLaneTickContext untyped {
        .request = RealtimeLaneTickRequest {
            .start_index = start_index,
            .sample_count = block_size_,
        },
        .compiled_fallback_tick_window = CompiledLaneTickRequest {
            .start_index = start_index,
            .end_index = start_index + block_size_,
            .sample_count = block_size_,
        },
        .compiled_sample_inputs = compiled_sample_inputs,
        .compiled_event_inputs = compiled_event_inputs,
        .realtime_sample_inputs = realtime_sample_inputs,
        .realtime_event_inputs = realtime_event_inputs,
        .output = std::move(output),
    };
    RealtimeLaneTickContext<TypeErasedLaneNode> ctx(untyped);
    tracked.node->tick_block_realtime(ctx);
}

std::span<Sample> TimelineExecution::realtime_sample_block_mutable_locked(LaneId lane)
{
    auto const it = realtime_sample_slot_by_lane_.find(lane);
    if (it == realtime_sample_slot_by_lane_.end()) {
        return {};
    }
    auto const offset = it->second * block_size_;
    return std::span<Sample>(realtime_sample_storage_).subspan(offset, block_size_);
}

std::span<Sample const> TimelineExecution::realtime_sample_block_locked(LaneId lane) const
{
    auto const it = realtime_sample_slot_by_lane_.find(lane);
    if (it == realtime_sample_slot_by_lane_.end()) {
        return {};
    }
    auto const offset = it->second * block_size_;
    return std::span<Sample const>(realtime_sample_storage_).subspan(offset, block_size_);
}

std::vector<Sample> const& TimelineExecution::ensure_compiled_sample_block_locked(LaneId lane, size_t start_index)
{
    auto &cache = compiled_sample_cache_[lane];
    if (cache.valid && cache.start_index == start_index) {
        return cache.samples;
    }
    auto it = tracked_lanes_.find(lane);
    if (it == tracked_lanes_.end() || it->second.node == nullptr) {
        static std::vector<Sample> empty;
        return empty;
    }
    execute_lane_locked(lane, start_index);
    return compiled_sample_cache_[lane].samples;
}

std::vector<TimedEvent> const& TimelineExecution::ensure_compiled_event_block_locked(LaneId lane, size_t start_index)
{
    auto &cache = compiled_event_cache_[lane];
    if (cache.valid && cache.start_index == start_index) {
        return cache.events;
    }
    auto it = tracked_lanes_.find(lane);
    if (it == tracked_lanes_.end() || it->second.node == nullptr) {
        static std::vector<TimedEvent> empty;
        return empty;
    }
    execute_lane_locked(lane, start_index);
    return compiled_event_cache_[lane].events;
}

bool TimelineExecution::has_task_graph_changes(TaskGraphUpdate const &update)
{
    return !update.to_create.empty() || !update.to_update.empty() || !update.to_delete.empty();
}
}
