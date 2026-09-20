#include <intravenous/graph_jit/event_retention_runtime.h>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace iv::graph_jit::detail {
namespace {
void insert_delayed_sequence(
    TimedEvent event,
    std::size_t loop_extra_latency,
    TimedEvent* target,
    std::size_t target_capacity,
    std::size_t& count) noexcept
{
    if (count >= target_capacity) return;
    event.time = static_cast<EventTime>(saturating_sample_index_add(
        static_cast<SampleIndex>(event.time), loop_extra_latency));
    if (count == 0 || target[count - 1].time <= event.time) {
        target[count++] = std::move(event);
        return;
    }
    auto const position = std::upper_bound(
        target,
        target + count,
        event.time,
        [](EventTime time, TimedEvent const& candidate) {
            return time < candidate.time;
        });
    std::move_backward(position, target + count, target + count + 1);
    *position = std::move(event);
    ++count;
}

void insert_delayed_ring(
    TimedEvent event,
    std::size_t loop_extra_latency,
    TimedEvent* target,
    std::size_t target_capacity,
    std::size_t read_index,
    std::size_t& write_index) noexcept
{
    if (write_index - read_index >= target_capacity) return;
    event.time = static_cast<EventTime>(saturating_sample_index_add(
        static_cast<SampleIndex>(event.time), loop_extra_latency));
    auto const mask = target_capacity - 1;
    if (write_index == read_index
        || target[(write_index - 1) & mask].time <= event.time) {
        target[write_index & mask] = std::move(event);
        ++write_index;
        return;
    }
    auto position = read_index;
    while (position != write_index
        && target[position & mask].time <= event.time) {
        ++position;
    }
    for (auto i = write_index; i != position; --i) {
        target[i & mask] = std::move(target[(i - 1) & mask]);
    }
    target[position & mask] = std::move(event);
    ++write_index;
}
} // namespace

extern "C" std::size_t iv_graph_jit_restore_event_carry(
    void const* carry_events,
    std::size_t carry_count,
    void* working_events,
    std::size_t working_capacity) noexcept
{
    if (carry_events == nullptr || working_events == nullptr) return 0;
    auto const bounded = std::min(carry_count, working_capacity);
    auto const* source = static_cast<TimedEvent const*>(carry_events);
    auto* target = static_cast<TimedEvent*>(working_events);
    std::copy_n(source, bounded, target);
    return bounded;
}

extern "C" std::size_t iv_graph_jit_commit_event_carry(
    void const* working_events,
    std::size_t working_count,
    std::size_t sample_index,
    std::size_t block_size,
    std::size_t retained_history_samples,
    std::size_t retained_latency_samples,
    void* carry_events,
    std::size_t carry_capacity) noexcept
{
    if (working_events == nullptr || carry_events == nullptr) return 0;

    auto const block_end = saturating_sample_index_add(
        static_cast<SampleIndex>(sample_index), block_size);
    auto const history = retained_history_samples > block_end
        ? static_cast<std::size_t>(block_end)
        : retained_history_samples;
    auto const window_begin = static_cast<std::size_t>(block_end) - history;
    auto const window_end = static_cast<std::size_t>(
        saturating_sample_index_add(block_end, retained_latency_samples));

    auto const* source = static_cast<TimedEvent const*>(working_events);
    auto* target = static_cast<TimedEvent*>(carry_events);
    std::size_t written = 0;
    for (std::size_t i = 0; i < working_count; ++i) {
        auto const time = static_cast<std::size_t>(source[i].time);
        if (time < window_begin || time >= window_end) continue;
        if (written == carry_capacity) break;
        target[written++] = source[i];
    }
    return written;
}

extern "C" std::size_t iv_graph_jit_prune_event_persistent_ring(
    void const* ring_events,
    std::size_t ring_capacity,
    std::size_t read_index,
    std::size_t write_index,
    std::size_t sample_index,
    std::size_t retained_history_samples) noexcept
{
    if (ring_events == nullptr || ring_capacity == 0) return write_index;

    auto const history = retained_history_samples > sample_index
        ? sample_index
        : retained_history_samples;
    auto const window_begin = sample_index - history;
    auto const* events = static_cast<TimedEvent const*>(ring_events);
    auto const mask = ring_capacity - 1;
    while (read_index != write_index) {
        auto const& event = events[read_index & mask];
        if (static_cast<std::size_t>(event.time) >= window_begin) break;
        ++read_index;
    }
    return read_index;
}

extern "C" void iv_graph_jit_append_event_feedback(
    void const* source_events,
    std::size_t source_begin_index,
    std::size_t source_end_index,
    std::size_t sample_index,
    std::size_t loop_extra_latency,
    void* ring_events,
    std::size_t ring_capacity,
    std::size_t* ring_read_index,
    std::size_t* ring_write_index) noexcept
{
    if (source_events == nullptr || ring_events == nullptr
        || ring_capacity == 0 || ring_read_index == nullptr
        || ring_write_index == nullptr) {
        return;
    }

    auto* target = static_cast<TimedEvent*>(ring_events);
    auto const* source = static_cast<TimedEvent const*>(source_events);
    auto const mask = ring_capacity - 1;
    auto read_index = *ring_read_index;
    auto write_index = *ring_write_index;

    while (read_index != write_index) {
        auto const& event = target[read_index & mask];
        if (static_cast<std::size_t>(event.time) >= sample_index) break;
        ++read_index;
    }

    source_begin_index = std::min(source_begin_index, source_end_index);
    for (std::size_t i = source_begin_index; i < source_end_index; ++i) {
        insert_delayed_ring(
            source[i],
            loop_extra_latency,
            target,
            ring_capacity,
            read_index,
            write_index);
    }
    *ring_read_index = read_index;
    *ring_write_index = write_index;
}

extern "C" void iv_graph_jit_append_event_feedback_sequence(
    void const* source_events,
    std::size_t source_begin_index,
    std::size_t source_end_index,
    std::size_t loop_extra_latency,
    void* target_events,
    std::size_t target_capacity,
    std::size_t* target_count) noexcept
{
    if (source_events == nullptr || target_events == nullptr
        || target_count == nullptr || target_capacity == 0) {
        return;
    }

    auto const* source = static_cast<TimedEvent const*>(source_events);
    auto* target = static_cast<TimedEvent*>(target_events);
    auto count = std::min(*target_count, target_capacity);
    source_begin_index = std::min(source_begin_index, source_end_index);
    for (std::size_t i = source_begin_index;
         i < source_end_index; ++i) {
        insert_delayed_sequence(
            source[i],
            loop_extra_latency,
            target,
            target_capacity,
            count);
    }
    *target_count = count;
}

extern "C" void iv_graph_jit_append_event_feedback_sequence_ring_source(
    void const* source_events,
    std::size_t source_capacity,
    std::size_t source_begin_index,
    std::size_t source_end_index,
    std::size_t loop_extra_latency,
    void* target_events,
    std::size_t target_capacity,
    std::size_t* target_count) noexcept
{
    if (source_events == nullptr || source_capacity == 0
        || target_events == nullptr || target_count == nullptr
        || target_capacity == 0) {
        return;
    }

    auto const* source = static_cast<TimedEvent const*>(source_events);
    auto* target = static_cast<TimedEvent*>(target_events);
    auto const source_mask = source_capacity - 1;
    auto count = std::min(*target_count, target_capacity);
    source_begin_index = std::min(source_begin_index, source_end_index);
    for (std::size_t i = source_begin_index;
         i < source_end_index; ++i) {
        insert_delayed_sequence(
            source[i & source_mask],
            loop_extra_latency,
            target,
            target_capacity,
            count);
    }
    *target_count = count;
}

extern "C" void iv_graph_jit_append_event_feedback_ring_source(
    void const* source_events,
    std::size_t source_capacity,
    std::size_t source_begin_index,
    std::size_t source_end_index,
    std::size_t sample_index,
    std::size_t loop_extra_latency,
    void* ring_events,
    std::size_t ring_capacity,
    std::size_t* ring_read_index,
    std::size_t* ring_write_index) noexcept
{
    if (source_events == nullptr || source_capacity == 0
        || ring_events == nullptr || ring_capacity == 0
        || ring_read_index == nullptr || ring_write_index == nullptr) {
        return;
    }

    auto* target = static_cast<TimedEvent*>(ring_events);
    auto const* source = static_cast<TimedEvent const*>(source_events);
    auto const source_mask = source_capacity - 1;
    auto const target_mask = ring_capacity - 1;
    auto read_index = *ring_read_index;
    auto write_index = *ring_write_index;

    while (read_index != write_index) {
        auto const& event = target[read_index & target_mask];
        if (static_cast<std::size_t>(event.time) >= sample_index) break;
        ++read_index;
    }

    source_begin_index = std::min(source_begin_index, source_end_index);
    for (std::size_t i = source_begin_index; i < source_end_index; ++i) {
        insert_delayed_ring(
            source[i & source_mask],
            loop_extra_latency,
            target,
            ring_capacity,
            read_index,
            write_index);
    }
    *ring_read_index = read_index;
    *ring_write_index = write_index;
}

} // namespace iv::graph_jit::detail
