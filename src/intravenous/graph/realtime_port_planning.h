#pragma once

#include <cstddef>

namespace iv {

// A realtime sample channel group or event stream has only three physical
// storage plans. Conversion, fan-in, fanout, feedback, and scheduling are
// operations over these plans rather than additional storage kinds.
enum class RealtimeBufferStorageKind {
    transient_stack,
    stack_with_persistent_carry,
    full_node_storage,
};

struct SampleConnectionStorageRequirements {
    // Frames newly processed by one generated-root invocation.
    std::size_t current_block_frames = 0;
    // History/latency frames whose values cross invocation boundaries.
    std::size_t retained_frames = 0;
    std::size_t channel_count = 1;
};

struct SampleConnectionStoragePlan {
    RealtimeBufferStorageKind kind =
        RealtimeBufferStorageKind::transient_stack;
};

[[nodiscard]] constexpr SampleConnectionStoragePlan
choose_sample_connection_storage_plan(
    SampleConnectionStorageRequirements const& requirements) noexcept
{
    if (requirements.retained_frames == 0
        || requirements.channel_count == 0) {
        return {RealtimeBufferStorageKind::transient_stack};
    }

    // Carry is worthwhile only while less than one current block must be
    // restored and committed. The physical planner may later choose full
    // persistent storage earlier when whole-group copy costs require it.
    return {
        requirements.retained_frames < requirements.current_block_frames
            ? RealtimeBufferStorageKind::stack_with_persistent_carry
            : RealtimeBufferStorageKind::full_node_storage,
    };
}

struct EventConnectionStorageRequirements {
    std::size_t current_window_samples = 0;
    std::size_t retained_window_samples = 0;
    // Exact unrounded maximum event counts derived from
    // max_events_per_sample for the current block and retained span.
    std::size_t current_event_capacity = 0;
    std::size_t retained_event_capacity = 0;
};

struct EventConnectionStoragePlan {
    RealtimeBufferStorageKind kind =
        RealtimeBufferStorageKind::transient_stack;
};

[[nodiscard]] constexpr EventConnectionStoragePlan
choose_event_connection_storage_plan(
    EventConnectionStorageRequirements const& requirements) noexcept
{
    if (requirements.retained_event_capacity == 0) {
        return {RealtimeBufferStorageKind::transient_stack};
    }

    // Both operands are produced from max_events_per_sample. No fixed event
    // count participates in this decision.
    return {
        requirements.retained_event_capacity
                < requirements.current_event_capacity
            ? RealtimeBufferStorageKind::stack_with_persistent_carry
            : RealtimeBufferStorageKind::full_node_storage,
    };
}

} // namespace iv
