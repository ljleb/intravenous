#include <intravenous/runtime/timeline_execution.h>

#include <algorithm>
#include <cmath>
#include <queue>
#include <stdexcept>
#include <unordered_set>

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

SampleStreamLayout sample_output_layout(LaneOutputConfig const& output)
{
    return std::visit([](auto const& config) -> SampleStreamLayout {
        using Config = std::remove_cvref_t<decltype(config)>;
        if constexpr (std::same_as<Config, RealtimeSampleLaneOutputConfig>
            || std::same_as<Config, CompiledSampleLaneOutputConfig>) {
            return config.sample_layout;
        } else {
            return SampleStreamLayout::planar;
        }
    }, output);
}

ChannelTypeId sample_channel_type_for(std::optional<ChannelTypeId> channel_type)
{
    return channel_type.value_or(ChannelTypeId::stereo);
}

ChannelLayout sample_output_channel_layout(
    LaneOutputConfig const& output,
    std::optional<ChannelTypeId> channel_type)
{
    return ChannelLayout {
        .channel_type = sample_channel_type_for(channel_type),
        .sample_layout = sample_output_layout(output),
    };
}

std::vector<Sample> convert_sample_block(
    SampleBlockView<Sample const> source,
    ChannelLayout target_layout)
{
    auto const target_samples = sample_storage_size(target_layout, source.frames());
    std::vector<Sample> converted(target_samples, Sample {});
    auto const plan = ChannelConversionRegistry::plan(source.channel_layout(), target_layout);
    plan.convert(source.samples().data(), converted.data(), source.frames());
    return converted;
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

void sort_and_merge_chunk_ranges(
    std::vector<CompiledSupportChunkRange>& ranges)
{
    std::ranges::sort(
        ranges,
        [](CompiledSupportChunkRange const& lhs,
           CompiledSupportChunkRange const& rhs) {
            if (lhs.start_chunk_index != rhs.start_chunk_index) {
                return lhs.start_chunk_index < rhs.start_chunk_index;
            }
            return lhs.end_chunk_index < rhs.end_chunk_index;
        });

    std::vector<CompiledSupportChunkRange> merged;
    merged.reserve(ranges.size());
    for (auto const& range : ranges) {
        if (range.end_chunk_index <= range.start_chunk_index) {
            continue;
        }
        if (merged.empty() || range.start_chunk_index > merged.back().end_chunk_index) {
            merged.push_back(range);
        } else {
            merged.back().end_chunk_index =
                std::max(merged.back().end_chunk_index, range.end_chunk_index);
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

std::vector<CompiledSupportChunkRange> derive_chunk_ranges(
    std::span<CompiledSupportRange const> sample_ranges,
    size_t chunk_size)
{
    std::vector<CompiledSupportChunkRange> chunk_ranges;
    chunk_ranges.reserve(sample_ranges.size());
    for (auto const& range : sample_ranges) {
        if (range.end_index <= range.start_index) {
            continue;
        }
        auto const start_chunk_index =
            chunk_index_for_sample(range.start_index, chunk_size);
        auto const end_chunk_index =
            chunk_index_for_sample(range.end_index - 1, chunk_size) + 1;
        chunk_ranges.push_back(CompiledSupportChunkRange {
            .start_chunk_index = start_chunk_index,
            .end_chunk_index = end_chunk_index,
        });
    }
    sort_and_merge_chunk_ranges(chunk_ranges);
    return chunk_ranges;
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

VersionedTaskGraphUpdate TimelineExecution::synchronize_from_graph(LaneGraph const &graph)
{
    std::vector<TrackedLane> lanes;
    graph.for_each_lane([&](LaneRecord const &record) {
        lanes.push_back(TrackedLane {
            .id = record.id,
            .node = &record.node,
            .output = record.output,
            .sample_channel_type = record.sample_channel_type,
            .inputs = graph.inputs_for(record.id),
            .external_task_dependencies = record.external_task_dependencies,
        });
    });

    std::scoped_lock lock(mutex_);
    return replace_all_lanes_locked(std::move(lanes));
}

VersionedTaskGraphUpdate TimelineExecution::handle_timeline_lanes_changed(TimelineLanesChanged const &change)
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
        if (change.lane_set_changed) {
            visited.reserve(visited.size() + tracked_lanes_.size());
            for (auto const &[lane, _] : tracked_lanes_) {
                visited.push_back(lane);
            }
            std::ranges::sort(visited, {}, &LaneId::value);
            visited.erase(
                std::unique(visited.begin(), visited.end()),
                visited.end());
        }
        change.visit_lanes(visited, [&](LaneId lane, TypeErasedLaneNode const &node, LaneOutputConfig const &output, std::optional<ChannelTypeId> sample_channel_type, std::vector<LaneInputConnection> const &inputs, std::vector<std::string> const &external_task_dependencies) {
            auto &tracked = tracked_lanes_[lane];
            bool const existed = tracked.id == lane && tracked.node != nullptr;
            tracked.id = lane;
            tracked.node = &node;
            tracked.output = output;
            tracked.sample_channel_type = sample_channel_type;
            tracked.inputs = inputs;
            tracked.external_task_dependencies = external_task_dependencies;

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

            auto build_depends_on = [&] {
                std::vector<std::string> depends_on;
                depends_on.reserve(inputs.size() + external_task_dependencies.size());
                for (auto const &input : inputs) {
                    depends_on.push_back(timeline_lane_task_id(input.source));
                }
                for (auto const &dependency : external_task_dependencies) {
                    depends_on.push_back(dependency);
                }
                return depends_on;
            };

            if (existed) {
                update.to_update.push_back(TaskUpdateRecord {
                    .id = timeline_lane_task_id(lane),
                    .depends_on = std::optional<std::vector<std::string>>(build_depends_on()),
                    .callback = tracked.callback,
                });
            } else {
                update.to_create.push_back(TaskRecord {
                    .id = timeline_lane_task_id(lane),
                    .depends_on = build_depends_on(),
                    .callback = tracked.callback,
                });
            }
        });
    }

    rebuild_runtime_storage_locked();
    rebuild_compiled_support_and_notify_locked();
    return VersionedTaskGraphUpdate{
        .version_index = change.version_index,
        .update = std::move(update),
    };
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

void TimelineExecution::pause()
{
    std::scoped_lock lock(mutex_);
    paused_ = true;
}

void TimelineExecution::resume(size_t start_index)
{
    std::scoped_lock lock(mutex_);
    current_start_index_ = start_index;
    paused_ = false;
}

bool TimelineExecution::is_paused() const
{
    std::scoped_lock lock(mutex_);
    return paused_;
}

void TimelineExecution::set_realtime_start_index(size_t start_index)
{
    std::scoped_lock lock(mutex_);
    if (paused_) {
        return;
    }
    current_start_index_ = start_index;
}

size_t TimelineExecution::realtime_start_index() const
{
    std::scoped_lock lock(mutex_);
    return current_start_index_;
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

BorrowedSampleBlock TimelineExecution::realtime_sample_block(LaneId lane) const
{
    std::scoped_lock lock(mutex_);
    auto const view = realtime_sample_block_locked(lane);
    return BorrowedSampleBlock{
        .samples = view.samples(),
        .channel_layout = view.channel_layout(),
        .frame_count = view.frames(),
    };
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

OwnedSampleBlock TimelineExecution::compiled_sample_block(LaneId lane, size_t start_index)
{
    std::scoped_lock lock(mutex_);
    return read_compiled_sample_block_locked(lane, start_index);
}

std::vector<TimedEvent> TimelineExecution::compiled_event_block(LaneId lane, size_t start_index)
{
    std::scoped_lock lock(mutex_);
    return ensure_compiled_event_block_locked(lane, start_index);
}

OwnedSampleBlock TimelineExecution::sparse_compiled_sample_window(
    LaneId lane,
    size_t first,
    size_t last,
    size_t count)
{
    if (count == 0 || last < first || block_size_ == 0) {
        return {};
    }
    std::scoped_lock lock(mutex_);
    auto const tracked_it = tracked_lanes_.find(lane);
    if (tracked_it == tracked_lanes_.end()) {
        return {};
    }
    auto const output_layout =
        sample_output_channel_layout(tracked_it->second.output, tracked_it->second.sample_channel_type);
    std::vector<Sample> result(sample_storage_size(output_layout, count), Sample {});
    auto result_view = SampleBlockView<Sample>(result, output_layout, count);
    auto const range = last - first;
    for (size_t i = 0; i < count; ++i) {
        size_t const source_index = (count == 1)
            ? first
            : first + static_cast<size_t>(std::llround(
                static_cast<double>(i) * static_cast<double>(range)
                / static_cast<double>(count - 1)));
        size_t const block_start = (source_index / block_size_) * block_size_;
        size_t const offset = source_index - block_start;
        auto const block = read_compiled_sample_block_locked(lane, block_start);
        auto const block_view = block.view();
        for (size_t channel = 0; channel < result_view.channels(); ++channel) {
            result_view.set(
                i,
                channel,
                offset < block_view.frames() ? block_view.get(offset, channel) : Sample {});
        }
    }
    return OwnedSampleBlock{
        .samples = std::move(result),
        .channel_layout = output_layout,
        .frame_count = count,
    };
}

std::vector<TimedEvent> TimelineExecution::compiled_events_in_range(
    LaneId lane,
    size_t first,
    size_t last)
{
    if (last < first || block_size_ == 0) {
        return {};
    }
    std::scoped_lock lock(mutex_);
    if (!tracked_lanes_.contains(lane)) {
        return {};
    }
    std::vector<TimedEvent> result;
    size_t block_start = (first / block_size_) * block_size_;
    size_t const last_block_start = (last / block_size_) * block_size_;
    while (block_start <= last_block_start) {
        auto const& block = ensure_compiled_event_block_locked(lane, block_start);
        for (auto const& event : block) {
            if (event.time >= first && event.time <= last) {
                result.push_back(event);
            }
        }
        if (block_start == last_block_start) {
            break;
        }
        block_start += block_size_;
    }
    return result;
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
    size_t next_sample_offset = 0;
    auto const order = topological_order_locked();
    for (auto const lane : order) {
        auto const &tracked = tracked_lanes_.at(lane);
        if (!is_realtime_sample_output(tracked.output)) {
            continue;
        }
        auto const channel_layout =
            sample_output_channel_layout(tracked.output, tracked.sample_channel_type);
        auto const sample_count = sample_storage_size(channel_layout, block_size_);
        RealtimeSamplePortDescriptor descriptor {
            .lane = lane,
            .sample_offset = next_sample_offset,
            .sample_count = sample_count,
            .channel_layout = channel_layout,
        };
        realtime_sample_slot_by_lane_[lane] = descriptor;
        realtime_sample_descriptors_.push_back(descriptor);
        next_sample_offset += sample_count;
    }
    realtime_sample_storage_.assign(next_sample_offset, 0.0f);

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
            auto chunk_ranges = derive_chunk_ranges(
                ranges,
                compiled_sample_cache_chunk_size_locked());
            compiled_support_by_lane_[lane] = CompiledSupportState {
                .sample_ranges = std::move(ranges),
                .chunk_ranges = std::move(chunk_ranges),
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

VersionedTaskGraphUpdate TimelineExecution::replace_all_lanes_locked(std::vector<TrackedLane> lanes)
{
    TaskGraphUpdate update;
    std::unordered_set<LaneId, LaneIdHash> incoming_lanes;
    incoming_lanes.reserve(lanes.size());
    for (auto const &tracked : lanes) {
        incoming_lanes.insert(tracked.id);
    }

    for (auto it = tracked_lanes_.begin(); it != tracked_lanes_.end();) {
        if (incoming_lanes.contains(it->first)) {
            ++it;
            continue;
        }
        update.to_delete.push_back(timeline_lane_task_id(it->first));
        callback_contexts_.erase(it->first);
        it = tracked_lanes_.erase(it);
    }

    realtime_sample_descriptors_.clear();
    realtime_sample_slot_by_lane_.clear();
    realtime_sample_storage_.clear();
    realtime_event_blocks_.clear();
    compiled_support_by_lane_.clear();
    compiled_sample_cache_.clear();
    compiled_event_cache_.clear();

    for (auto &tracked : lanes) {
        bool const existed = tracked_lanes_.contains(tracked.id);
        auto &stored = tracked_lanes_[tracked.id];
        stored = std::move(tracked);
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
        depends_on.reserve(stored.inputs.size() + stored.external_task_dependencies.size());
        for (auto const &input : stored.inputs) {
            depends_on.push_back(timeline_lane_task_id(input.source));
        }
        for (auto const &dependency : stored.external_task_dependencies) {
            depends_on.push_back(dependency);
        }
        if (existed) {
            update.to_update.push_back(TaskUpdateRecord {
                .id = timeline_lane_task_id(stored.id),
                .depends_on = std::move(depends_on),
                .callback = stored.callback,
            });
        } else {
            update.to_create.push_back(TaskRecord {
                .id = timeline_lane_task_id(stored.id),
                .depends_on = std::move(depends_on),
                .callback = stored.callback,
            });
        }
    }

    rebuild_runtime_storage_locked();
    rebuild_compiled_support_and_notify_locked();
    return VersionedTaskGraphUpdate{
        .version_index = 0,
        .update = std::move(update),
    };
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

    auto input_layout_for = [&](LanePortId input) {
        SampleStreamLayout sample_layout = SampleStreamLayout::planar;
        if (input.domain == LanePortDomain::compiled) {
            sample_layout = tracked.node->compiled_sample_inputs()[input.ordinal].sample_layout;
        } else {
            sample_layout = tracked.node->realtime_sample_inputs()[input.ordinal].sample_layout;
        }
        return ChannelLayout {
            .channel_type = sample_channel_type_for(tracked.sample_channel_type),
            .sample_layout = sample_layout,
        };
    };

    for (auto const &connection : tracked.inputs) {
        if (!tracked_lanes_.contains(connection.source)) {
            continue;
        }
        auto const &source = tracked_lanes_.at(connection.source);
        if (connection.input.kind == PortKind::sample) {
            auto const target_layout = input_layout_for(connection.input);
            if (connection.input.domain == LanePortDomain::compiled) {
                auto source_block = read_compiled_sample_block_locked(connection.source, start_index);
                auto source_view = source_block.view();
                if (source_view.channel_layout() != target_layout) {
                    compiled_sample_input_blocks.push_back(convert_sample_block(source_view, target_layout));
                    compiled_sample_inputs[connection.input.ordinal].sources.push_back(
                        SampleBlockView<Sample const>(
                            compiled_sample_input_blocks.back(),
                            target_layout,
                            block_size_));
                } else {
                    compiled_sample_input_blocks.push_back(std::move(source_block.samples));
                    compiled_sample_inputs[connection.input.ordinal].sources.push_back(
                        SampleBlockView<Sample const>(
                            compiled_sample_input_blocks.back(),
                            target_layout,
                            block_size_));
                }
                compiled_sample_inputs[connection.input.ordinal].channel_layout = target_layout;
                compiled_sample_inputs[connection.input.ordinal].frame_count = block_size_;
            } else if (is_compiled_sample_output(source.output)) {
                auto source_block = read_compiled_sample_block_locked(connection.source, start_index);
                auto source_view = source_block.view();
                if (source_view.channel_layout() != target_layout) {
                    compiled_sample_input_blocks.push_back(convert_sample_block(source_view, target_layout));
                    realtime_sample_inputs[connection.input.ordinal].sources.push_back(
                        SampleBlockView<Sample const>(
                            compiled_sample_input_blocks.back(),
                            target_layout,
                            block_size_));
                } else {
                    compiled_sample_input_blocks.push_back(std::move(source_block.samples));
                    realtime_sample_inputs[connection.input.ordinal].sources.push_back(
                        SampleBlockView<Sample const>(
                            compiled_sample_input_blocks.back(),
                            target_layout,
                            block_size_));
                }
                realtime_sample_inputs[connection.input.ordinal].channel_layout = target_layout;
                realtime_sample_inputs[connection.input.ordinal].frame_count = block_size_;
            } else {
                auto const source_view = realtime_sample_block_locked(connection.source);
                if (source_view.channel_layout() != target_layout) {
                    compiled_sample_input_blocks.push_back(convert_sample_block(source_view, target_layout));
                    realtime_sample_inputs[connection.input.ordinal].sources.push_back(
                        SampleBlockView<Sample const>(
                            compiled_sample_input_blocks.back(),
                            target_layout,
                            block_size_));
                } else {
                    realtime_sample_inputs[connection.input.ordinal].sources.push_back(source_view);
                }
                realtime_sample_inputs[connection.input.ordinal].channel_layout = target_layout;
                realtime_sample_inputs[connection.input.ordinal].frame_count = block_size_;
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
        std::ranges::fill(samples.samples(), 0.0f);
        output = RealtimeSampleLaneOutput(samples);
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

SampleBlockView<Sample> TimelineExecution::realtime_sample_block_mutable_locked(LaneId lane)
{
    auto const it = realtime_sample_slot_by_lane_.find(lane);
    if (it == realtime_sample_slot_by_lane_.end()) {
        return {};
    }
    auto const& descriptor = it->second;
    return SampleBlockView<Sample>(
        std::span<Sample>(realtime_sample_storage_).subspan(
            descriptor.sample_offset,
            descriptor.sample_count),
        descriptor.channel_layout,
        block_size_);
}

SampleBlockView<Sample const> TimelineExecution::realtime_sample_block_locked(LaneId lane) const
{
    auto const it = realtime_sample_slot_by_lane_.find(lane);
    if (it == realtime_sample_slot_by_lane_.end()) {
        return {};
    }
    auto const& descriptor = it->second;
    return SampleBlockView<Sample const>(
        std::span<Sample const>(realtime_sample_storage_).subspan(
            descriptor.sample_offset,
            descriptor.sample_count),
        descriptor.channel_layout,
        block_size_);
}

std::span<CompiledSupportRange const> TimelineExecution::compiled_support_ranges_locked(LaneId lane) const
{
    auto const it = compiled_support_by_lane_.find(lane);
    if (it == compiled_support_by_lane_.end()) {
        return {};
    }
    return it->second.sample_ranges;
}

std::span<CompiledSupportChunkRange const>
TimelineExecution::compiled_support_chunk_ranges_locked(LaneId lane) const
{
    auto const it = compiled_support_by_lane_.find(lane);
    if (it == compiled_support_by_lane_.end()) {
        return {};
    }
    return it->second.chunk_ranges;
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

    auto input_layout_for = [&](LanePortId input) {
        SampleStreamLayout sample_layout = SampleStreamLayout::planar;
        if (input.domain == LanePortDomain::compiled) {
            sample_layout = tracked.node->compiled_sample_inputs()[input.ordinal].sample_layout;
        } else {
            sample_layout = tracked.node->realtime_sample_inputs()[input.ordinal].sample_layout;
        }
        return ChannelLayout {
            .channel_type = sample_channel_type_for(tracked.sample_channel_type),
            .sample_layout = sample_layout,
        };
    };

    for (auto const& connection : tracked.inputs) {
        if (!tracked_lanes_.contains(connection.source)) {
            continue;
        }
        auto const& source = tracked_lanes_.at(connection.source);
        if (connection.input.kind == PortKind::sample) {
            auto const target_layout = input_layout_for(connection.input);
            if (connection.input.domain == LanePortDomain::compiled) {
                auto source_block = read_compiled_sample_block_locked(connection.source, chunk_start);
                auto source_view = source_block.view();
                if (source_view.channel_layout() != target_layout) {
                    compiled_sample_input_blocks.push_back(convert_sample_block(source_view, target_layout));
                    compiled_sample_inputs[connection.input.ordinal].sources.push_back(
                        SampleBlockView<Sample const>(
                            compiled_sample_input_blocks.back(),
                            target_layout,
                            chunk_size));
                } else {
                    compiled_sample_input_blocks.push_back(std::move(source_block.samples));
                    compiled_sample_inputs[connection.input.ordinal].sources.push_back(
                        SampleBlockView<Sample const>(
                            compiled_sample_input_blocks.back(),
                            target_layout,
                            chunk_size));
                }
                compiled_sample_inputs[connection.input.ordinal].channel_layout = target_layout;
                compiled_sample_inputs[connection.input.ordinal].frame_count = chunk_size;
            } else if (is_compiled_sample_output(source.output)) {
                auto source_block = read_compiled_sample_block_locked(connection.source, chunk_start);
                auto source_view = source_block.view();
                if (source_view.channel_layout() != target_layout) {
                    compiled_sample_input_blocks.push_back(convert_sample_block(source_view, target_layout));
                    realtime_sample_inputs[connection.input.ordinal].sources.push_back(
                        SampleBlockView<Sample const>(
                            compiled_sample_input_blocks.back(),
                            target_layout,
                            chunk_size));
                } else {
                    compiled_sample_input_blocks.push_back(std::move(source_block.samples));
                    realtime_sample_inputs[connection.input.ordinal].sources.push_back(
                        SampleBlockView<Sample const>(
                            compiled_sample_input_blocks.back(),
                            target_layout,
                            chunk_size));
                }
                realtime_sample_inputs[connection.input.ordinal].channel_layout = target_layout;
                realtime_sample_inputs[connection.input.ordinal].frame_count = chunk_size;
            } else {
                auto const source_view = realtime_sample_block_locked(connection.source);
                if (source_view.channel_layout() != target_layout) {
                    compiled_sample_input_blocks.push_back(convert_sample_block(source_view, target_layout));
                    realtime_sample_inputs[connection.input.ordinal].sources.push_back(
                        SampleBlockView<Sample const>(
                            compiled_sample_input_blocks.back(),
                            target_layout,
                            chunk_size));
                } else {
                    realtime_sample_inputs[connection.input.ordinal].sources.push_back(source_view);
                }
                realtime_sample_inputs[connection.input.ordinal].channel_layout = target_layout;
                realtime_sample_inputs[connection.input.ordinal].frame_count = chunk_size;
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
    auto const output_layout =
        sample_output_channel_layout(tracked.output, tracked.sample_channel_type);
    chunk.assign(sample_storage_size(output_layout, chunk_size), 0.0f);
    LaneOutputView output = CompiledSampleLaneOutput {
        .samples = &chunk,
        .window_start_index = chunk_start,
        .clamp_to_window = true,
        .channel_layout = output_layout,
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

OwnedSampleBlock TimelineExecution::read_compiled_sample_block_locked(LaneId lane, size_t start_index)
{
    auto const tracked_it = tracked_lanes_.find(lane);
    if (tracked_it == tracked_lanes_.end()) {
        return {};
    }
    auto const output_layout =
        sample_output_channel_layout(tracked_it->second.output, tracked_it->second.sample_channel_type);
    auto const channels = channel_count(output_layout);
    std::vector<Sample> result(sample_storage_size(output_layout, block_size_), 0.0f);
    auto const sample_ranges = compiled_support_ranges_locked(lane);
    auto const chunk_ranges = compiled_support_chunk_ranges_locked(lane);
    if (sample_ranges.empty() || chunk_ranges.empty()) {
        return OwnedSampleBlock{
            .samples = std::move(result),
            .channel_layout = output_layout,
            .frame_count = block_size_,
        };
    }

    auto& cache = compiled_sample_cache_[lane];
    size_t const request_start = start_index;
    size_t const request_end = start_index + block_size_;
    auto const chunk_size = compiled_sample_cache_chunk_size_locked();
    auto const request_start_chunk_index =
        chunk_index_for_sample(request_start, chunk_size);
    auto const request_end_chunk_index =
        chunk_index_for_sample(request_end - 1, chunk_size) + 1;

    for (auto const& chunk_range : chunk_ranges) {
        if (chunk_range.end_chunk_index <= request_start_chunk_index) {
            continue;
        }
        if (chunk_range.start_chunk_index >= request_end_chunk_index) {
            break;
        }

        auto chunk_index =
            std::max(chunk_range.start_chunk_index, request_start_chunk_index);
        auto const last_chunk_index =
            std::min(chunk_range.end_chunk_index, request_end_chunk_index);
        for (; chunk_index < last_chunk_index; ++chunk_index) {
            if (!cache.chunks.contains(chunk_index)) {
                execute_compiled_sample_chunk_locked(lane, chunk_index);
            }
            auto const chunk_it = cache.chunks.find(chunk_index);
            if (chunk_it == cache.chunks.end()) {
                continue;
            }

            auto const chunk_start = chunk_start_index(chunk_index, chunk_size);
            auto const chunk_end = chunk_start + chunk_size;
            auto const copy_start = std::max(request_start, chunk_start);
            auto const copy_end = std::min(request_end, chunk_end);
            if (copy_end <= copy_start) {
                continue;
            }
            for (auto const& sample_range : sample_ranges) {
                if (sample_range.end_index <= copy_start) {
                    continue;
                }
                if (sample_range.start_index >= copy_end) {
                    break;
                }

                auto const supported_copy_start =
                    std::max(copy_start, sample_range.start_index);
                auto const supported_copy_end =
                    std::min(copy_end, sample_range.end_index);
                if (supported_copy_end <= supported_copy_start) {
                    continue;
                }

                if (output_layout.sample_layout == SampleStreamLayout::interleaved) {
                    auto const source_offset =
                        (supported_copy_start - chunk_start) * channels;
                    auto const dest_offset =
                        (supported_copy_start - request_start) * channels;
                    auto const count =
                        (supported_copy_end - supported_copy_start) * channels;
                    std::ranges::copy(
                        std::span<Sample const>(chunk_it->second).subspan(source_offset, count),
                        result.begin() + static_cast<std::ptrdiff_t>(dest_offset));
                } else {
                    auto const source_frame_offset = supported_copy_start - chunk_start;
                    auto const dest_frame_offset = supported_copy_start - request_start;
                    auto const frame_count = supported_copy_end - supported_copy_start;
                    for (size_t channel = 0; channel < channels; ++channel) {
                        auto const source_offset = channel * chunk_size + source_frame_offset;
                        auto const dest_offset = channel * block_size_ + dest_frame_offset;
                        std::ranges::copy(
                            std::span<Sample const>(chunk_it->second).subspan(source_offset, frame_count),
                            result.begin() + static_cast<std::ptrdiff_t>(dest_offset));
                    }
                }
            }
        }
    }
    return OwnedSampleBlock{
        .samples = std::move(result),
        .channel_layout = output_layout,
        .frame_count = block_size_,
    };
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
