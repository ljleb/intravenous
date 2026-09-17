#include <intravenous/graph_jit/event_retention_runtime.h>

#include <algorithm>
#include <cstddef>

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

} // namespace iv::graph_jit::detail
