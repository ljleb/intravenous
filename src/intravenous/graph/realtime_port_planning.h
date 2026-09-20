#pragma once

#include <array>
#include <cstddef>
#include <limits>

namespace iv {

// A realtime sample channel group or event stream has only three physical
// storage plans. Conversion, fan-in, fanout, feedback, and scheduling are
// operations over these plans rather than additional storage kinds.
enum class RealtimeBufferStorageKind {
    transient_stack,
    stack_with_persistent_carry,
    full_node_storage,
};

// Whole-group storage selection compares fixed-capacity candidates in byte
// units. The default weights deliberately preserve the original block-relative
// crossover: carry wins while retained < current, and full persistent wins at
// equality. Candidate-specific copy counts can move the crossover earlier;
// correctness never depends on the weights.
struct RealtimeStorageCostModel {
    // A hard per-candidate bound. Candidates whose invocation-local footprint
    // exceeds it are illegal rather than silently spilling to dynamic storage.
    std::size_t stack_budget_bytes = std::numeric_limits<std::size_t>::max();

    std::size_t copied_byte_weight = 1;
    std::size_t ring_addressed_byte_weight = 0;
    std::size_t stack_footprint_byte_weight = 0;
    std::size_t persistent_footprint_byte_weight = 2;
};

// Operation work that is known before physical residence is selected. Counts
// are payload values rather than bytes so sample channels and event sequences
// can share the same policy. Invariant work is reported for every candidate;
// candidate-specific work lets topology planning account for producer-home
// fan-in, conversion/fanout materialization, and similar alternatives without
// introducing new storage kinds.
struct RealtimeStorageOperationCounts {
    std::size_t invariant_copied_values = 0;
    std::size_t transient_extra_copied_values = 0;
    std::size_t carry_extra_copied_values = 0;
    std::size_t full_extra_copied_values = 0;
    std::size_t full_ring_addressed_values = 0;
};

struct RealtimeStorageCandidateCost {
    bool legal = false;
    std::size_t copied_bytes = 0;
    std::size_t ring_addressed_bytes = 0;
    std::size_t stack_bytes = 0;
    std::size_t persistent_bytes = 0;
    std::size_t weighted_cost = std::numeric_limits<std::size_t>::max();
};

struct RealtimeStorageCandidateCosts {
    RealtimeStorageCandidateCost transient_stack{};
    RealtimeStorageCandidateCost stack_with_persistent_carry{};
    RealtimeStorageCandidateCost full_node_storage{};

    [[nodiscard]] constexpr RealtimeStorageCandidateCost const& for_kind(
        RealtimeBufferStorageKind kind) const noexcept
    {
        switch (kind) {
        case RealtimeBufferStorageKind::transient_stack:
            return transient_stack;
        case RealtimeBufferStorageKind::stack_with_persistent_carry:
            return stack_with_persistent_carry;
        case RealtimeBufferStorageKind::full_node_storage:
            return full_node_storage;
        }
        return full_node_storage;
    }
};

namespace detail {

[[nodiscard]] constexpr std::size_t saturating_add(
    std::size_t lhs,
    std::size_t rhs) noexcept
{
    auto const maximum = std::numeric_limits<std::size_t>::max();
    return rhs > maximum - lhs ? maximum : lhs + rhs;
}

[[nodiscard]] constexpr std::size_t saturating_multiply(
    std::size_t lhs,
    std::size_t rhs) noexcept
{
    if (lhs == 0 || rhs == 0) return 0;
    auto const maximum = std::numeric_limits<std::size_t>::max();
    return lhs > maximum / rhs ? maximum : lhs * rhs;
}

[[nodiscard]] constexpr std::size_t values_to_bytes(
    std::size_t values,
    std::size_t value_size_bytes) noexcept
{
    return saturating_multiply(values, value_size_bytes);
}

[[nodiscard]] constexpr std::size_t weighted_sum(
    RealtimeStorageCandidateCost const& cost,
    RealtimeStorageCostModel const& model) noexcept
{
    auto total = saturating_multiply(cost.copied_bytes, model.copied_byte_weight);
    total = saturating_add(
        total,
        saturating_multiply(
            cost.ring_addressed_bytes, model.ring_addressed_byte_weight));
    total = saturating_add(
        total,
        saturating_multiply(
            cost.stack_bytes, model.stack_footprint_byte_weight));
    return saturating_add(
        total,
        saturating_multiply(
            cost.persistent_bytes, model.persistent_footprint_byte_weight));
}

[[nodiscard]] constexpr RealtimeStorageCandidateCost make_candidate_cost(
    bool legal,
    std::size_t copied_values,
    std::size_t ring_addressed_values,
    std::size_t stack_values,
    std::size_t persistent_values,
    std::size_t value_size_bytes,
    RealtimeStorageCostModel const& model) noexcept
{
    RealtimeStorageCandidateCost result{
        .legal = legal,
        .copied_bytes = values_to_bytes(copied_values, value_size_bytes),
        .ring_addressed_bytes = values_to_bytes(
            ring_addressed_values, value_size_bytes),
        .stack_bytes = values_to_bytes(stack_values, value_size_bytes),
        .persistent_bytes = values_to_bytes(
            persistent_values, value_size_bytes),
    };
    result.legal = result.legal && result.stack_bytes <= model.stack_budget_bytes;
    if (result.legal) result.weighted_cost = weighted_sum(result, model);
    return result;
}

struct RealtimeStorageSelectionInput {
    std::size_t current_values = 0;
    std::size_t retained_values = 0;
    std::size_t value_size_bytes = 1;
    RealtimeStorageOperationCounts operations{};
};

struct RealtimeStorageSelectionResult {
    RealtimeBufferStorageKind kind = RealtimeBufferStorageKind::transient_stack;
    RealtimeStorageCandidateCosts costs{};
};

[[nodiscard]] constexpr RealtimeStorageSelectionResult choose_realtime_storage(
    RealtimeStorageSelectionInput const& input,
    RealtimeStorageCostModel const& model) noexcept
{
    auto const working_values = saturating_add(
        input.current_values, input.retained_values);
    auto const invariant = input.operations.invariant_copied_values;

    RealtimeStorageSelectionResult result;
    result.costs.transient_stack = make_candidate_cost(
        input.retained_values == 0,
        saturating_add(
            invariant, input.operations.transient_extra_copied_values),
        0,
        input.current_values,
        0,
        input.value_size_bytes,
        model);

    auto const retention_copies = saturating_multiply(
        input.retained_values, std::size_t{2});
    result.costs.stack_with_persistent_carry = make_candidate_cost(
        input.retained_values != 0
            && input.retained_values < input.current_values,
        saturating_add(
            saturating_add(invariant, input.operations.carry_extra_copied_values),
            retention_copies),
        0,
        working_values,
        input.retained_values,
        input.value_size_bytes,
        model);

    result.costs.full_node_storage = make_candidate_cost(
        true,
        saturating_add(invariant, input.operations.full_extra_copied_values),
        input.operations.full_ring_addressed_values,
        0,
        working_values,
        input.value_size_bytes,
        model);

    // Prefer transient on an exact tie, then full persistent, then carry. The
    // full-before-carry tie break preserves the established retained==current
    // crossover while still allowing candidate-specific work to select full
    // persistent earlier.
    result.kind = RealtimeBufferStorageKind::full_node_storage;
    auto best = result.costs.full_node_storage.weighted_cost;
    if (result.costs.transient_stack.legal
        && result.costs.transient_stack.weighted_cost <= best) {
        result.kind = RealtimeBufferStorageKind::transient_stack;
        best = result.costs.transient_stack.weighted_cost;
    }
    if (result.costs.stack_with_persistent_carry.legal
        && result.costs.stack_with_persistent_carry.weighted_cost < best) {
        result.kind = RealtimeBufferStorageKind::stack_with_persistent_carry;
    }
    return result;
}

} // namespace detail

struct SampleConnectionStorageRequirements {
    // Frames newly processed by one generated-root invocation.
    std::size_t current_block_frames = 0;
    // History/latency frames whose values cross invocation boundaries.
    std::size_t retained_frames = 0;
    std::size_t channel_count = 1;
    std::size_t value_size_bytes = 1;
    RealtimeStorageOperationCounts operations{};
};

struct SampleConnectionStoragePlan {
    RealtimeBufferStorageKind kind =
        RealtimeBufferStorageKind::transient_stack;
    RealtimeStorageCandidateCosts candidate_costs{};
};

[[nodiscard]] constexpr SampleConnectionStoragePlan
choose_sample_connection_storage_plan(
    SampleConnectionStorageRequirements const& requirements,
    RealtimeStorageCostModel const& model = {}) noexcept
{
    if (requirements.channel_count == 0) {
        return {RealtimeBufferStorageKind::transient_stack, {}};
    }
    auto const current_values = detail::saturating_multiply(
        requirements.current_block_frames, requirements.channel_count);
    auto const retained_values = detail::saturating_multiply(
        requirements.retained_frames, requirements.channel_count);
    auto const selected = detail::choose_realtime_storage(
        detail::RealtimeStorageSelectionInput{
            .current_values = current_values,
            .retained_values = retained_values,
            .value_size_bytes = requirements.value_size_bytes,
            .operations = requirements.operations,
        },
        model);
    return {
        .kind = selected.kind,
        .candidate_costs = selected.costs,
    };
}

struct EventConnectionStorageRequirements {
    std::size_t current_window_samples = 0;
    std::size_t retained_window_samples = 0;
    // Exact unrounded maximum event counts derived from
    // max_events_per_sample for the current block and retained span.
    std::size_t current_event_capacity = 0;
    std::size_t retained_event_capacity = 0;
    std::size_t value_size_bytes = 1;
    RealtimeStorageOperationCounts operations{};
};

struct EventConnectionStoragePlan {
    RealtimeBufferStorageKind kind =
        RealtimeBufferStorageKind::transient_stack;
    RealtimeStorageCandidateCosts candidate_costs{};
};

[[nodiscard]] constexpr EventConnectionStoragePlan
choose_event_connection_storage_plan(
    EventConnectionStorageRequirements const& requirements,
    RealtimeStorageCostModel const& model = {}) noexcept
{
    auto const selected = detail::choose_realtime_storage(
        detail::RealtimeStorageSelectionInput{
            .current_values = requirements.current_event_capacity,
            .retained_values = requirements.retained_event_capacity,
            .value_size_bytes = requirements.value_size_bytes,
            .operations = requirements.operations,
        },
        model);
    return {
        .kind = selected.kind,
        .candidate_costs = selected.costs,
    };
}

} // namespace iv
