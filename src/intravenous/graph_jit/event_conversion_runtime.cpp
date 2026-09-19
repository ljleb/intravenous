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


extern "C" std::size_t iv_graph_jit_merge_event_sequence(
    void* target_events,
    std::size_t target_capacity,
    std::size_t target_read_index,
    std::size_t target_write_index,
    void const* source_events,
    std::size_t source_capacity,
    std::size_t source_count) noexcept
{
    if (target_events == nullptr || source_events == nullptr
        || (target_capacity != 0 && !is_power_of_2(target_capacity))) {
        return target_write_index;
    }
    if (target_write_index < target_read_index || target_capacity == 0) {
        return target_write_index;
    }

    auto const target_count = std::min(
        target_write_index - target_read_index, target_capacity);
    auto const bounded_source_count = std::min(source_count, source_capacity);
    if (bounded_source_count > target_capacity - target_count) {
        // Physical planning guarantees enough aggregate capacity. Preserve the
        // already-valid target rather than silently manufacturing a partial
        // ordering if that invariant is ever violated.
        return target_write_index;
    }

    auto* target = static_cast<TimedEvent*>(target_events);
    auto const* source = static_cast<TimedEvent const*>(source_events);
    auto const mask = target_capacity - 1;
    auto target_remaining = target_count;
    auto source_remaining = bounded_source_count;
    auto output_remaining = target_count + bounded_source_count;

    while (output_remaining != 0) {
        bool take_target = false;
        if (source_remaining == 0) {
            take_target = true;
        } else if (target_remaining != 0) {
            auto const& target_event = target[
                (target_read_index + target_remaining - 1) & mask];
            auto const& source_event = source[source_remaining - 1];
            // On equal timestamps take the newly merged source while walking
            // backwards, leaving the existing target event earlier in the
            // final stable order.
            take_target = target_event.time > source_event.time;
        }

        auto const output_index =
            (target_read_index + output_remaining - 1) & mask;
        if (take_target) {
            target[output_index] = target[
                (target_read_index + target_remaining - 1) & mask];
            --target_remaining;
        } else {
            target[output_index] = source[source_remaining - 1];
            --source_remaining;
        }
        --output_remaining;
    }
    return target_read_index + target_count + bounded_source_count;
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
