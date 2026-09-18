#include <intravenous/graph_jit/event_conversion_runtime.h>

#include <algorithm>
#include <array>

namespace iv::graph_jit::detail {

extern "C" std::size_t iv_graph_jit_convert_event_sequence(
    std::uint32_t source_type,
    std::uint32_t target_type,
    std::uint32_t step0,
    std::uint32_t step1,
    std::uint32_t step2,
    std::size_t step_count,
    void const* source_events,
    std::size_t source_count,
    void* target_events,
    std::size_t target_capacity) noexcept
{
    if (step_count > EventConversionPlan::max_steps
        || source_type >= static_cast<std::uint32_t>(EventTypeId::count)
        || target_type >= static_cast<std::uint32_t>(EventTypeId::count)
        || source_events == nullptr || target_events == nullptr) {
        return 0;
    }

    EventConversionPlan plan{
        .source_type = static_cast<EventTypeId>(source_type),
        .target_type = static_cast<EventTypeId>(target_type),
        .steps = {
            static_cast<EventConversionStepId>(step0),
            static_cast<EventConversionStepId>(step1),
            static_cast<EventConversionStepId>(step2),
        },
        .step_count = step_count,
    };
    auto const* source = static_cast<TimedEvent const*>(source_events);
    auto* target = static_cast<TimedEvent*>(target_events);

    std::size_t written = 0;
    for (std::size_t i = 0; i < source_count; ++i) {
        EventConversionRegistry::instance().convert(
            plan,
            source[i],
            [&](TimedEvent const& converted) {
                if (written < target_capacity) {
                    target[written++] = converted;
                }
            });
    }
    return written;
}


extern "C" std::size_t iv_graph_jit_materialize_event_sequence(
    std::uint32_t source_type,
    std::uint32_t target_type,
    std::uint32_t step0,
    std::uint32_t step1,
    std::uint32_t step2,
    std::size_t step_count,
    void const* source_events,
    std::size_t source_capacity,
    std::size_t source_read_index,
    std::size_t source_write_index,
    SampleIndex block_index,
    std::size_t block_size,
    std::size_t history_samples,
    void* target_events,
    std::size_t target_capacity) noexcept
{
    if (step_count > EventConversionPlan::max_steps
        || source_type >= static_cast<std::uint32_t>(EventTypeId::count)
        || target_type >= static_cast<std::uint32_t>(EventTypeId::count)
        || source_events == nullptr || target_events == nullptr
        || (source_capacity != 0 && !is_power_of_2(source_capacity))) {
        return 0;
    }
    if (source_capacity == 0 || target_capacity == 0
        || source_write_index < source_read_index) {
        return 0;
    }

    EventConversionPlan plan{
        .source_type = static_cast<EventTypeId>(source_type),
        .target_type = static_cast<EventTypeId>(target_type),
        .steps = {
            static_cast<EventConversionStepId>(step0),
            static_cast<EventConversionStepId>(step1),
            static_cast<EventConversionStepId>(step2),
        },
        .step_count = step_count,
    };
    if (!EventConversionRegistry::is_nonexpanding(plan)) {
        return 0;
    }

    auto const window = realtime_port_window(
        block_index, block_size, history_samples, 0);
    auto const* source = static_cast<TimedEvent const*>(source_events);
    auto* target = static_cast<TimedEvent*>(target_events);
    auto const available = std::min(
        source_write_index - source_read_index, source_capacity);
    auto const mask = source_capacity - 1;

    std::size_t written = 0;
    for (std::size_t offset = 0; offset < available; ++offset) {
        auto const& event = source[(source_read_index + offset) & mask];
        auto const time = static_cast<SampleIndex>(event.time);
        if (!window.contains(time)) {
            continue;
        }
        EventConversionRegistry::instance().convert(
            plan,
            event,
            [&](TimedEvent const& converted) {
                // Non-expanding conversion plus target_capacity >=
                // source_capacity makes this an internal sizing invariant.
                if (written < target_capacity) {
                    target[written++] = converted;
                }
            });
    }
    return written;
}

} // namespace iv::graph_jit::detail
