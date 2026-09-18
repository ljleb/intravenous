#include <intravenous/graph_jit/event_retention_runtime.h>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace iv::graph_jit::detail {

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

extern "C" std::size_t iv_graph_jit_append_event_feedback(
    void const* source_events,
    std::size_t source_count,
    std::size_t sample_index,
    std::size_t block_size,
    std::size_t loop_extra_latency,
    void* ring_events,
    std::size_t ring_capacity,
    std::size_t read_index,
    std::size_t write_index) noexcept
{
    if (source_events == nullptr || ring_events == nullptr || ring_capacity == 0) {
        return write_index;
    }

    auto const block_begin = static_cast<SampleIndex>(sample_index);
    auto const block_end = saturating_sample_index_add(block_begin, block_size);
    auto const* source = static_cast<TimedEvent const*>(source_events);
    auto* target = static_cast<TimedEvent*>(ring_events);
    auto const mask = ring_capacity - 1;
    for (std::size_t i = 0; i < source_count; ++i) {
        auto const time = static_cast<SampleIndex>(source[i].time);
        if (time < block_begin || time >= block_end) continue;
        if (write_index - read_index >= ring_capacity) break;
        auto delayed = source[i];
        delayed.time = static_cast<EventTime>(
            saturating_sample_index_add(time, loop_extra_latency));
        target[write_index & mask] = std::move(delayed);
        ++write_index;
    }
    return write_index;
}

} // namespace iv::graph_jit::detail
