#pragma once

#include <intravenous/graph_jit/realtime_port_state_plan.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iv::graph_jit {

// How one inherited semantic port-state interval can be represented by the
// prepared successor. This is selected from the predecessor's recoverable
// views and the successor's steady views; it is not a raw-region copy policy.
enum class InheritedRealtimePortStateRealization : std::uint8_t {
    // The successor's immutable/fresh state already has the inherited value.
    already_represented,
    // The successor owns one private steady storage view that can receive the
    // inherited interval during transition preparation.
    transfer_to_steady_storage,
    // The steady realization aliases, derives, or omits this semantic state;
    // the transition realization must own temporary storage for it.
    transition_only_storage,
    // The predecessor retained no recoverable representation of this state.
    // Cutover cannot proceed until an earlier realization retains a source.
    predecessor_state_unavailable,
};

// One surviving semantic port-state piece across a graph-generation boundary.
// Indices refer to the immutable metadata of the predecessor/current compiled
// graphs. The inherited interval is relative to the cutover position: history
// occupies a negative interval ending at zero, while authored output latency
// occupies a non-negative interval beginning at zero.
struct RealtimePortStateTransition {
    std::size_t previous_requirement_index = 0;
    std::size_t current_requirement_index = 0;
    std::size_t previous_realization_index = 0;
    std::size_t current_realization_index = 0;
    std::int64_t inherited_begin = 0;
    std::int64_t inherited_end = 0;
    std::size_t inherited_extent_samples = 0;
    std::size_t newly_exposed_extent_samples = 0;
    std::size_t discarded_extent_samples = 0;
    InheritedRealtimePortStateRealization inherited_state_realization =
        InheritedRealtimePortStateRealization::transition_only_storage;
};

// Concrete cold descriptor for transferring one recoverable predecessor
// sample interval into one non-aliased successor steady-storage view. Source
// views belong to the predecessor NodeStorage; target belongs to the successor.
struct SampleRealtimePortStateTransfer {
    std::size_t transition_index = 0;
    SampleRealtimePortStateAccess source_access =
        SampleRealtimePortStateAccess::callback_only;
    std::vector<SampleRealtimePortStateStorageView> source_storage{};
    std::optional<SampleRealtimePortStateMaterialization>
        source_materialization{};
    // Channel of source_materialization.target_layout represented by this
    // semantic state. Ignored when source_access is not materialized.
    std::size_t source_materialization_channel = 0;
    SampleRealtimePortStateStorageView target_storage{};
    // Complete successor interval. It is initialized to silence before the
    // inherited sub-interval is transferred from predecessor storage.
    std::int64_t target_begin = 0;
    std::int64_t target_end = 0;
    std::int64_t inherited_begin = 0;
    std::int64_t inherited_end = 0;
};

// Concrete cold descriptor for selecting/converting one predecessor event
// interval into one non-aliased successor steady-storage view. Event payloads
// retain absolute timestamps, so the inherited interval is also the filter
// applied at the cutover position.
struct EventRealtimePortStateTransfer {
    std::size_t transition_index = 0;
    EventRealtimePortStateAccess source_access =
        EventRealtimePortStateAccess::callback_only;
    EventRealtimePortStateStorageView source_storage{};
    std::optional<std::size_t> source_index{};
    std::optional<EventConversionPlan> source_conversion{};
    std::size_t source_history_samples = 0;
    bool source_selects_invocation_window = false;
    EventRealtimePortStateStorageView target_storage{};
    // Complete successor interval; events outside the inherited sub-interval
    // are authoritatively absent after reconstruction.
    std::int64_t target_begin = 0;
    std::int64_t target_end = 0;
    std::int64_t inherited_begin = 0;
    std::int64_t inherited_end = 0;
};

// Cold semantic reconciliation for one prospective generation cutover. This
// intentionally contains no raw-region transfers: physical transition
// realization consumes these matches after stable node-port ownership has
// already determined what must survive.
struct RealtimePortStateTransitionPlan {
    std::vector<RealtimePortStateTransition> sample_states{};
    std::vector<RealtimePortStateTransition> event_states{};
    std::vector<SampleRealtimePortStateTransfer> sample_transfers{};
    std::vector<EventRealtimePortStateTransfer> event_transfers{};
};

// Fully checked cutover program. Construction validates every cold descriptor,
// storage extent, and supported realization before the realtime boundary. The
// executor can therefore run the contained transfers without allocation or a
// recoverable failure path.
class PreparedRealtimePortStateTransition {
    RealtimePortStateTransitionPlan plan_{};
    std::size_t previous_storage_size_ = 0;
    std::size_t current_storage_size_ = 0;

    PreparedRealtimePortStateTransition(
        RealtimePortStateTransitionPlan plan,
        std::size_t previous_storage_size,
        std::size_t current_storage_size);

    friend std::expected<PreparedRealtimePortStateTransition, std::string>
    prepare_realtime_port_state_transfers(
        RealtimePortStateTransitionPlan,
        std::span<std::byte const>,
        std::span<std::byte>);
    friend void execute_realtime_port_state_transfers(
        PreparedRealtimePortStateTransition const&,
        std::span<std::byte const>,
        std::span<std::byte>,
        GlobalIndex) noexcept;

public:
    PreparedRealtimePortStateTransition(
        PreparedRealtimePortStateTransition&&) noexcept = default;
    PreparedRealtimePortStateTransition& operator=(
        PreparedRealtimePortStateTransition&&) noexcept = default;
    PreparedRealtimePortStateTransition(
        PreparedRealtimePortStateTransition const&) = delete;
    PreparedRealtimePortStateTransition& operator=(
        PreparedRealtimePortStateTransition const&) = delete;

    [[nodiscard]] RealtimePortStateTransitionPlan const& plan() const noexcept
    {
        return plan_;
    }
};

[[nodiscard]] std::expected<RealtimePortStateTransitionPlan, std::string>
plan_realtime_port_state_transition(
    RealtimePortStateRequirements const& previous_requirements,
    RealtimePortStateRealizations const& previous_realizations,
    RealtimePortStateRequirements const& current_requirements,
    RealtimePortStateRealizations const& current_realizations);

// Validates and owns the complete bounded cutover program before activation.
// Transition-only and unavailable inherited state are rejected until their
// suffix realizations have been added to the plan.
[[nodiscard]] std::expected<PreparedRealtimePortStateTransition, std::string>
prepare_realtime_port_state_transfers(
    RealtimePortStateTransitionPlan plan,
    std::span<std::byte const> previous_storage,
    std::span<std::byte> current_storage);

// Executes already-prepared transfers between canonical persistent sample
// storage and event rings. sample_index is the first sample processed by the
// successor generation. All validation, allocation, and diagnostic creation
// happens in prepare_realtime_port_state_transfers().
void execute_realtime_port_state_transfers(
    PreparedRealtimePortStateTransition const& transition,
    std::span<std::byte const> previous_storage,
    std::span<std::byte> current_storage,
    GlobalIndex sample_index) noexcept;

} // namespace iv::graph_jit
