#pragma once

#include <intravenous/sample.h>

#include <cstddef>
#include <optional>

namespace iv {

// Physical implementation choices for one already-analyzed realtime sample
// connection group. Correctness requirements are derived before this chooser;
// this enum is deliberately only a storage/execution policy decision.
enum class SampleConnectionImplementationKind {
    direct,
    transient_materialization,
    compact_persistent_carry,
    persistent_ring,
    feedback_ring,
    external_boundary,
};

struct SampleConnectionImplementationRequirements {
    // True only when producer and all relevant consumers can share/fuse the
    // same representation without an intermediate materialization.
    bool direct_implementation_legal = false;
    bool requires_materialization = false;
    bool feedback = false;
    bool external_boundary = false;

    // Frames whose values must survive a kernel invocation. Zero means the
    // connection has no cross-call retention requirement.
    std::size_t retained_frames = 0;
    std::size_t channel_count = 1;
    std::size_t value_size_bytes = sizeof(Sample);
};

struct SampleConnectionCostModel {
    // Initial deliberately-simple crossover. Compact carry copies the small
    // retained prefix into/out of the current-block representation; larger
    // retained payloads use a ring. This is policy, not semantics.
    std::size_t compact_carry_max_bytes = 16 * 1024;
};

[[nodiscard]] constexpr SampleConnectionImplementationKind
choose_sample_connection_implementation(
    SampleConnectionImplementationRequirements const& requirements,
    SampleConnectionCostModel const& cost_model = {}) noexcept
{
    if (requirements.external_boundary) {
        return SampleConnectionImplementationKind::external_boundary;
    }
    if (requirements.feedback) {
        return SampleConnectionImplementationKind::feedback_ring;
    }
    if (requirements.retained_frames == 0) {
        if (requirements.direct_implementation_legal
            && !requirements.requires_materialization) {
            return SampleConnectionImplementationKind::direct;
        }
        return SampleConnectionImplementationKind::transient_materialization;
    }

    auto exceeds_compact_budget = [&] {
        if (requirements.channel_count == 0
            || requirements.value_size_bytes == 0) {
            return false;
        }
        auto const max = cost_model.compact_carry_max_bytes;
        if (requirements.retained_frames > max / requirements.channel_count) {
            return true;
        }
        auto const frame_values = requirements.retained_frames
            * requirements.channel_count;
        return frame_values > max / requirements.value_size_bytes;
    };

    return exceeds_compact_budget()
        ? SampleConnectionImplementationKind::persistent_ring
        : SampleConnectionImplementationKind::compact_persistent_carry;
}

// Event storage choices mirror sample choices, but the temporal window does
// not imply a semantic maximum event count. The estimate below is explicitly a
// cost-model input; an unknown estimate therefore selects the conservative
// persistent-ring representation.
enum class EventConnectionImplementationKind {
    direct,
    transient_sequence,
    compact_persistent_carry,
    persistent_ring,
    feedback_ring,
    external_boundary,
};

struct EventConnectionImplementationRequirements {
    bool direct_implementation_legal = false;
    bool requires_materialization = false;
    bool feedback = false;
    bool external_boundary = false;

    // Nonzero when events must remain observable across kernel invocations.
    std::size_t retained_window_samples = 0;
    std::optional<std::size_t> estimated_retained_events {};
};

struct EventConnectionCostModel {
    std::size_t compact_carry_max_events = 64;
};

[[nodiscard]] constexpr EventConnectionImplementationKind
choose_event_connection_implementation(
    EventConnectionImplementationRequirements const& requirements,
    EventConnectionCostModel const& cost_model = {}) noexcept
{
    if (requirements.external_boundary) {
        return EventConnectionImplementationKind::external_boundary;
    }
    if (requirements.feedback) {
        return EventConnectionImplementationKind::feedback_ring;
    }
    if (requirements.retained_window_samples == 0) {
        if (requirements.direct_implementation_legal
            && !requirements.requires_materialization) {
            return EventConnectionImplementationKind::direct;
        }
        return EventConnectionImplementationKind::transient_sequence;
    }

    if (!requirements.estimated_retained_events
        || *requirements.estimated_retained_events
            > cost_model.compact_carry_max_events) {
        return EventConnectionImplementationKind::persistent_ring;
    }
    return EventConnectionImplementationKind::compact_persistent_carry;
}

} // namespace iv
