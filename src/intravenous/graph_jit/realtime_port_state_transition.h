#pragma once

#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/graph_jit/realtime_port_state_plan.h>
#include <intravenous/node/layout.h>

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
    // The value type defines no valid migration for the old/new resolved
    // shapes. The successor's initialized state is the complete result.
    fresh_state,
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
    // True only when target_storage is a private region appended after the
    // successor's steady NodeStorage boundary.
    bool target_transition_only = false;
    SampleRealtimePortStateAccess source_access =
        SampleRealtimePortStateAccess::callback_only;
    std::vector<SampleRealtimePortStateStorageView> source_storage{};
    std::optional<SampleRealtimePortStateMaterialization>
        source_materialization{};
    // Channel of source_materialization.target_layout represented by this
    // semantic state. Ignored when source_access is not materialized.
    std::size_t source_materialization_channel = 0;
    std::vector<float> source_composition_coefficients{};
    std::optional<SampleRealtimePortStateInvocationCombination>
        source_invocation_combination{};
    SampleRealtimePortStateStorageView target_storage{};
    // Complete successor interval. It is initialized to silence before the
    // inherited sub-interval is transferred from predecessor storage.
    std::int64_t target_begin = 0;
    std::int64_t target_end = 0;
    std::int64_t inherited_begin = 0;
    std::int64_t inherited_end = 0;
};

// Concrete cold descriptor for selecting/converting one predecessor event
// interval into successor steady or transition storage. Several descriptors
// may merge disjoint roles or producer contributions into one aggregate.
// Ephemeral Sequential payloads are rebased across a transport seam before
// this cutover-relative filter is applied.
struct EventRealtimePortStateTransfer {
    std::size_t transition_index = 0;
    // True when the same semantic state must also populate another steady
    // representation. This descriptor does not satisfy the one-primary-
    // transfer requirement for transition_index.
    bool supplemental_target = false;
    bool target_transition_only = false;
    EventRealtimePortStateAccess source_access =
        EventRealtimePortStateAccess::callback_only;
    EventRealtimePortStateStorageView source_storage{};
    std::optional<std::size_t> source_index{};
    std::optional<EventConversionPlan> source_conversion{};
    std::size_t source_history_samples = 0;
    bool source_selects_invocation_window = false;
    std::optional<EventRealtimePortStateInvocationCombination>
        source_invocation_combination{};
    std::optional<EventConversionPlan> source_invocation_conversion{};
    EventRealtimePortStateStorageView target_storage{};
    // Populated when several semantic output states share one indexed target
    // aggregate. Every inserted event receives this stable producer index.
    std::optional<std::size_t> target_index{};
    // Successor interval initialized by this descriptor. Descriptors which
    // share a physical target contribute the union of these intervals.
    std::int64_t target_begin = 0;
    std::int64_t target_end = 0;
    std::int64_t inherited_begin = 0;
    std::int64_t inherited_end = 0;
    // Cold preparation replaces this with the earliest semantic position of
    // every transfer that appends into the same physical target. The realtime
    // merge uses that shared origin so history and future roles have one
    // chronological ordering across the cutover boundary.
    std::int64_t target_order_begin = 0;
};

// One finite transition-root lifetime measured in the same hidden Sequential
// domain that addresses the affected sample state. The executor snapshots the
// cursor at activation and expires the transition only after value_count
// transported values have actually advanced, independent of GlobalIndex
// discontinuities and project-rate assumptions.
struct SampleRealtimePortStateExpiryCursor {
    std::size_t node_storage_offset = 0;
    std::uint64_t value_count = 0;
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
    std::vector<SampleRealtimePortStateExpiryCursor> sample_expiry_cursors{};
    // Event time is project-rate. This horizon is advanced by processed block
    // extents, never by differences between visible GlobalIndex values.
    std::uint64_t event_expiry_samples = 0;
};

// One successor layout after cold transition storage specialization. The
// steady prefix is unchanged; plan transfers which still carry the
// transition_only_storage classification target private regions in its suffix.
struct RealtimePortStateTransitionStorage {
    NodeLayout node_layout{};
    RealtimePortStateTransitionPlan plan{};
};

// The predecessor TickInvocationWorkspace's Sequential playback bindings,
// indexed by TickRuntimePlan slot. invocation_playback transfer sources read
// these globally addressed windows in place at cutover. The bindings are
// fixed when the workspace is constructed, so cold preparation may inspect
// their geometry while the predecessor is still running.
struct RealtimePortStatePlaybackSlots {
    std::span<ReflectedContinuousInputPortBinding const> sequential_samples{};
    std::span<ReflectedEventInputPortBinding const> sequential_events{};
};

// Fully checked cutover program. Construction validates every cold descriptor,
// storage extent, and supported realization before the realtime boundary. The
// executor can therefore run the contained transfers without allocation or a
// recoverable failure path.
class PreparedRealtimePortStateTransition {
    RealtimePortStateTransitionPlan plan_{};
    std::size_t previous_storage_size_ = 0;
    std::size_t current_storage_size_ = 0;
    std::size_t playback_sample_slot_count_ = 0;
    std::size_t playback_event_slot_count_ = 0;

    PreparedRealtimePortStateTransition(
        RealtimePortStateTransitionPlan plan,
        std::size_t previous_storage_size,
        std::size_t current_storage_size,
        RealtimePortStatePlaybackSlots const& playback);

    friend std::expected<PreparedRealtimePortStateTransition, std::string>
    prepare_realtime_port_state_transfers(
        RealtimePortStateTransitionPlan,
        std::span<std::byte const>,
        std::span<std::byte>,
        RealtimePortStatePlaybackSlots const&);
    friend void execute_realtime_port_state_transfers(
        PreparedRealtimePortStateTransition const&,
        std::span<std::byte const>,
        std::span<std::byte>,
        GlobalIndex,
        RealtimePortStatePlaybackSlots const&,
        std::optional<GlobalIndex>) noexcept;

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

// Realizes sample transition state whose successor steady binding cannot
// directly receive inherited values. Each source and target retains its own
// generation-owned Sequential pace cursor, so cutover remains independent of
// node-visible transport jumps and supports a change of resolved dyadic pace.
// Disjoint roles/channels of one
// authored port may share a private suffix ring while all steady offsets stay
// fixed. A predecessor input produced by one retained linear channel
// materialization can be reconstructed during the prepared cutover. Direct,
// materialized, and composed successor callback views can then be mirrored into
// the suffix by transition lowering. A pure invocation-supplied successor is
// staged into that suffix from TickInvocationCall until expiry; a pure
// invocation-supplied predecessor is read from its final playback slots.
// Mixed realtime/background callback-only inputs remain nonrecoverable.
[[nodiscard]] std::expected<RealtimePortStateTransitionStorage, std::string>
realize_realtime_port_state_transition_storage(
    NodeLayout steady_layout,
    RealtimePortStateTransitionPlan plan,
    RealtimePortStateRealizations const& previous_realizations,
    RealtimePortStateRequirements const& current_requirements,
    RealtimePortStateRealizations const& current_realizations);

// Validates and owns the complete bounded cutover program before activation.
// A transition-only state is accepted only after suffix realization has added
// its one concrete transfer; unavailable inherited state remains an error.
// playback names the predecessor's slots when any transfer source is
// invocation_playback.
[[nodiscard]] std::expected<PreparedRealtimePortStateTransition, std::string>
prepare_realtime_port_state_transfers(
    RealtimePortStateTransitionPlan plan,
    std::span<std::byte const> previous_storage,
    std::span<std::byte> current_storage,
    RealtimePortStatePlaybackSlots const& playback = {});

// Executes already-prepared transfers between canonical persistent sample
// storage and event rings. sample_index is the first sample processed by the
// successor generation. playback must be the slots given to preparation.
// playback_end is the exclusive end of the predecessor's final invocation
// window, the playback counterpart of a Sequential cursor; it is absent when
// the predecessor never invoked, in which case playback sources contribute
// no inherited values. All validation, allocation, and diagnostic creation
// happens in prepare_realtime_port_state_transfers().
void execute_realtime_port_state_transfers(
    PreparedRealtimePortStateTransition const& transition,
    std::span<std::byte const> previous_storage,
    std::span<std::byte> current_storage,
    GlobalIndex sample_index,
    RealtimePortStatePlaybackSlots const& playback = {},
    std::optional<GlobalIndex> playback_end = std::nullopt) noexcept;

} // namespace iv::graph_jit
