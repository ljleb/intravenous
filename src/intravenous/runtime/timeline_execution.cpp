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

void sort_and_merge_ranges(std::vector<CompiledSupportRange>& ranges)
{
    std::ranges::sort(ranges, [](CompiledSupportRange const& lhs, CompiledSupportRange const& rhs) {
        if (lhs.start_index != rhs.start_index) {
            return lhs.start_index < rhs.start_index;
        }
        return lhs.end_index < rhs.end_index;
    });

    std::vector<CompiledSupportRange> merged;
    merged.reserve(ranges.size());
    for (auto const& range : ranges) {
        if (range.end_index <= range.start_index) {
            continue;
        }
        if (merged.empty() || range.start_index > merged.back().end_index) {
            merged.push_back(range);
        } else {
            merged.back().end_index = std::max(merged.back().end_index, range.end_index);
        }
    }
    ranges.swap(merged);
}

bool ranges_intersect(
    std::span<CompiledSupportRange const> ranges,
    size_t start_index,
    size_t end_index)
{
    for (auto const& range : ranges) {
        if (range.end_index <= start_index) {
            continue;
        }
        if (range.start_index >= end_index) {
            break;
        }
        return true;
    }
    return false;
}

size_t chunk_start_index(size_t chunk_index, size_t chunk_size)
{
    return chunk_index * chunk_size;
}

size_t chunk_index_for_sample(size_t sample_index, size_t chunk_size)
{
    return sample_index / chunk_size;
}
}

TimelineExecution::TimelineExecution(
    size_t block_size,
    size_t compiled_sample_cache_chunk_size_multiplier) :
    block_size_(block_size)
{
    if (block_size_ == 0) {
        throw std::runtime_error("timeline execution block size must be > 0");
    }
    set_compiled_sample_cache_chunk_size_multiplier(
        compiled_sample_cache_chunk_size_multiplier);
}

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
        compiled_support_by_lane_.erase(lane);
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
    rebuild_compiled_support_and_notify_locked();
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

void TimelineExecution::set_compiled_sample_cache_chunk_size_multiplier(size_t multiplier)
{
    if (multiplier == 0) {
        throw std::runtime_error(
            "compiled sample cache chunk size multiplier must be > 0");
    }
    std::scoped_lock lock(mutex_);
    if (compiled_sample_cache_chunk_size_multiplier_ == multiplier) {
        return;
    }
    compiled_sample_cache_chunk_size_multiplier_ = multiplier;
    compiled_sample_cache_.clear();
}

size_t TimelineExecution::compiled_sample_cache_chunk_size_multiplier() const
{
    std::scoped_lock lock(mutex_);
    return compiled_sample_cache_chunk_size_multiplier_;
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
    return read_compiled_sample_block_locked(lane, start_index);
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

void TimelineExecution::rebuild_compiled_support_and_notify_locked()
{
    compiled_support_by_lane_.clear();
    auto const order = topological_order_locked();
    for (auto const lane : order) {
        auto tracked_it = tracked_lanes_.find(lane);
        if (tracked_it == tracked_lanes_.end() || tracked_it->second.node == nullptr) {
            continue;
        }
        auto& tracked = tracked_it->second;

        if (supports_compiled_support_ranges(*tracked.node)) {
            std::vector<CompiledSupportLaneInput> compiled_sample_inputs;
            std::vector<CompiledSupportLaneInput> compiled_event_inputs;
            compiled_sample_inputs.resize(tracked.node->compiled_sample_inputs().size());
            compiled_event_inputs.resize(tracked.node->compiled_event_inputs().size());

            for (auto const& connection : tracked.inputs) {
                auto const source_it = tracked_lanes_.find(connection.source);
                if (source_it == tracked_lanes_.end()) {
                    continue;
                }
                auto const source_support = compiled_support_ranges_locked(connection.source);
                if (source_support.empty()) {
                    continue;
                }
                if (connection.input.kind == PortKind::sample) {
                    if (connection.input.ordinal < compiled_sample_inputs.size()) {
                        compiled_sample_inputs[connection.input.ordinal].sources.push_back(source_support);
                    }
                } else {
                    if (connection.input.ordinal < compiled_event_inputs.size()) {
                        compiled_event_inputs[connection.input.ordinal].sources.push_back(source_support);
                    }
                }
            }

            UntypedCompiledSupportContext untyped_support {
                .compiled_sample_inputs = compiled_sample_inputs,
                .compiled_event_inputs = compiled_event_inputs,
            };
            CompiledSupportContext<TypeErasedLaneNode> support_ctx(untyped_support);
            auto ranges = tracked.node->compiled_support_ranges(support_ctx);
            sort_and_merge_ranges(ranges);
            compiled_support_by_lane_[lane] = CompiledSupportState {
                .ranges = std::move(ranges),
            };
        }

        if (tracked.node->has_on_inputs_changed()) {
            std::vector<LaneInputConnectivityDescription> connectivity;
            connectivity.reserve(tracked.inputs.size());
            for (auto const& connection : tracked.inputs) {
                auto source_output_domain = LanePortDomain::realtime;
                std::span<CompiledSupportRange const> source_support {};
                auto const source_it = tracked_lanes_.find(connection.source);
                if (source_it != tracked_lanes_.end()) {
                    source_output_domain = lane_output_domain(source_it->second.output);
                    if (source_output_domain == LanePortDomain::compiled) {
                        source_support = compiled_support_ranges_locked(connection.source);
                    }
                }
                connectivity.push_back(LaneInputConnectivityDescription {
                    .source_lane_value = connection.source.value,
                    .source_output_domain = source_output_domain,
                    .kind = connection.input.kind,
                    .input_ordinal = connection.input.ordinal,
                    .compiled_support_ranges = source_support,
                });
            }
            UntypedInputsChangedContext untyped_inputs_changed {
                .inputs = connectivity,
            };
            InputsChangedContext<TypeErasedLaneNode> inputs_changed_ctx(untyped_inputs_changed);
            tracked.node->on_inputs_changed(inputs_changed_ctx);
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
    compiled_support_by_lane_.clear();
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
    rebuild_compiled_support_and_notify_locked();
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
        (void)read_compiled_sample_block_locked(lane, start_index);
        return;
    } else if (is_compiled_event_output(tracked.output)) {
        if (!compiled_support_intersects_request_locked(lane, start_index, block_size_)) {
            auto &cache = compiled_event_cache_[lane];
            cache.start_index = start_index;
            cache.events.clear();
            cache.valid = true;
            return;
        }
        auto const cache_it = compiled_event_cache_.find(lane);
        if (cache_it != compiled_event_cache_.end() && cache_it->second.valid && cache_it->second.start_index == start_index) {
            return;
        }
    }

    std::vector<CompiledSampleLaneInput> compiled_sample_inputs;
    std::vector<CompiledEventLaneInput> compiled_event_inputs;
    std::vector<RealtimeSampleLaneInput> realtime_sample_inputs;
    std::vector<RealtimeEventLaneInput> realtime_event_inputs;
    std::vector<std::vector<Sample>> compiled_sample_input_blocks;

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
                compiled_sample_input_blocks.push_back(
                    read_compiled_sample_block_locked(connection.source, start_index));
                compiled_sample_inputs[connection.input.ordinal].sources.push_back(
                    std::span<Sample const>(compiled_sample_input_blocks.back()));
            } else if (is_compiled_sample_output(source.output)) {
                compiled_sample_input_blocks.push_back(
                    read_compiled_sample_block_locked(connection.source, start_index));
                realtime_sample_inputs[connection.input.ordinal].block_override =
                    std::span<Sample const>(compiled_sample_input_blocks.back());
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

std::span<CompiledSupportRange const> TimelineExecution::compiled_support_ranges_locked(LaneId lane) const
{
    auto const it = compiled_support_by_lane_.find(lane);
    if (it == compiled_support_by_lane_.end()) {
        return {};
    }
    return it->second.ranges;
}

bool TimelineExecution::compiled_support_intersects_request_locked(
    LaneId lane,
    size_t start_index,
    size_t sample_count) const
{
    auto const ranges = compiled_support_ranges_locked(lane);
    if (ranges.empty()) {
        return false;
    }
    return ranges_intersect(ranges, start_index, start_index + sample_count);
}

size_t TimelineExecution::compiled_sample_cache_chunk_size_locked() const
{
    return block_size_ * compiled_sample_cache_chunk_size_multiplier_;
}

void TimelineExecution::execute_compiled_sample_chunk_locked(LaneId lane, size_t chunk_index)
{
    auto it = tracked_lanes_.find(lane);
    if (it == tracked_lanes_.end() || it->second.node == nullptr) {
        return;
    }
    auto& tracked = it->second;

    std::vector<CompiledSampleLaneInput> compiled_sample_inputs;
    std::vector<CompiledEventLaneInput> compiled_event_inputs;
    std::vector<RealtimeSampleLaneInput> realtime_sample_inputs;
    std::vector<RealtimeEventLaneInput> realtime_event_inputs;
    std::vector<std::vector<Sample>> compiled_sample_input_blocks;

    compiled_sample_inputs.resize(tracked.node->compiled_sample_inputs().size());
    compiled_event_inputs.resize(tracked.node->compiled_event_inputs().size());
    realtime_sample_inputs.resize(tracked.node->realtime_sample_inputs().size());
    realtime_event_inputs.resize(tracked.node->realtime_event_inputs().size());

    auto const chunk_size = compiled_sample_cache_chunk_size_locked();
    auto const chunk_start = chunk_start_index(chunk_index, chunk_size);

    for (auto const& connection : tracked.inputs) {
        if (!tracked_lanes_.contains(connection.source)) {
            continue;
        }
        auto const& source = tracked_lanes_.at(connection.source);
        if (connection.input.kind == PortKind::sample) {
            if (connection.input.domain == LanePortDomain::compiled) {
                compiled_sample_input_blocks.push_back(
                    read_compiled_sample_block_locked(connection.source, chunk_start));
                compiled_sample_inputs[connection.input.ordinal].sources.push_back(
                    std::span<Sample const>(compiled_sample_input_blocks.back()));
            } else if (is_compiled_sample_output(source.output)) {
                compiled_sample_input_blocks.push_back(
                    read_compiled_sample_block_locked(connection.source, chunk_start));
                realtime_sample_inputs[connection.input.ordinal].block_override =
                    std::span<Sample const>(compiled_sample_input_blocks.back());
                realtime_sample_inputs[connection.input.ordinal].active_start_index = chunk_start;
                realtime_sample_inputs[connection.input.ordinal].active_count = chunk_size;
                realtime_sample_inputs[connection.input.ordinal].has_source = true;
            } else {
                auto const values = realtime_sample_block_locked(connection.source);
                realtime_sample_inputs[connection.input.ordinal].block_override = values;
                realtime_sample_inputs[connection.input.ordinal].active_start_index = chunk_start;
                realtime_sample_inputs[connection.input.ordinal].active_count = chunk_size;
                realtime_sample_inputs[connection.input.ordinal].has_source = true;
            }
        } else {
            if (connection.input.domain == LanePortDomain::compiled) {
                auto const& events = ensure_compiled_event_block_locked(connection.source, chunk_start);
                compiled_event_inputs[connection.input.ordinal].sources.push_back(std::span<TimedEvent const>(events));
            } else if (is_compiled_event_output(source.output)) {
                auto const& events = ensure_compiled_event_block_locked(connection.source, chunk_start);
                realtime_event_inputs[connection.input.ordinal].block_override = std::span<TimedEvent const>(events);
                realtime_event_inputs[connection.input.ordinal].active_start_index = chunk_start;
                realtime_event_inputs[connection.input.ordinal].active_count = chunk_size;
            } else {
                auto const& events = realtime_event_blocks_[connection.source];
                realtime_event_inputs[connection.input.ordinal].block_override = events;
                realtime_event_inputs[connection.input.ordinal].active_start_index = chunk_start;
                realtime_event_inputs[connection.input.ordinal].active_count = chunk_size;
            }
        }
    }

    auto& chunk = compiled_sample_cache_[lane].chunks[chunk_index];
    chunk.assign(chunk_size, 0.0f);
    LaneOutputView output = CompiledSampleLaneOutput {
        .samples = &chunk,
        .window_start_index = chunk_start,
        .clamp_to_window = true,
    };

    UntypedRealtimeLaneTickContext untyped {
        .request = RealtimeLaneTickRequest {
            .start_index = chunk_start,
            .sample_count = block_size_,
        },
        .compiled_fallback_tick_window = CompiledLaneTickRequest {
            .start_index = chunk_start,
            .end_index = chunk_start + chunk_size,
            .sample_count = chunk_size,
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

std::vector<Sample> TimelineExecution::read_compiled_sample_block_locked(LaneId lane, size_t start_index)
{
    std::vector<Sample> result(block_size_, 0.0f);
    auto const ranges = compiled_support_ranges_locked(lane);
    if (ranges.empty()) {
        return result;
    }

    auto& cache = compiled_sample_cache_[lane];
    size_t const request_start = start_index;
    size_t const request_end = start_index + block_size_;

    for (auto const& range : ranges) {
        if (range.end_index <= request_start) {
            continue;
        }
        if (range.start_index >= request_end) {
            break;
        }

        auto const overlap_start = std::max(range.start_index, request_start);
        auto const overlap_end = std::min(range.end_index, request_end);
        if (overlap_end <= overlap_start) {
            continue;
        }

        auto const chunk_size = compiled_sample_cache_chunk_size_locked();
        auto chunk_index = chunk_index_for_sample(overlap_start, chunk_size);
        auto const last_chunk_index =
            chunk_index_for_sample(overlap_end - 1, chunk_size);
        for (; chunk_index <= last_chunk_index; ++chunk_index) {
            auto const chunk_start = chunk_start_index(
                chunk_index,
                chunk_size);
            auto const chunk_end = chunk_start + chunk_size;
            auto const copy_start = std::max(overlap_start, chunk_start);
            auto const copy_end = std::min(overlap_end, chunk_end);
            if (copy_end <= copy_start) {
                continue;
            }

            if (!cache.chunks.contains(chunk_index)) {
                execute_compiled_sample_chunk_locked(lane, chunk_index);
            }
            auto const chunk_it = cache.chunks.find(chunk_index);
            if (chunk_it == cache.chunks.end()) {
                continue;
            }

            auto const source_offset = copy_start - chunk_start;
            auto const dest_offset = copy_start - request_start;
            auto const count = copy_end - copy_start;
            std::ranges::copy(
                std::span<Sample const>(chunk_it->second).subspan(source_offset, count),
                result.begin() + static_cast<std::ptrdiff_t>(dest_offset));
        }
    }
    return result;
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
