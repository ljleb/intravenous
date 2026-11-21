#pragma once

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
// graphs. Intervals are relative to the cutover position. A Tick output's
// window, its last history + latency positions, occupies a negative interval
// ending at zero, whatever its stream kind. A Sequential input's window is
// [-history, T): its history, then its in-flight values, where T is the position
// at which its source's values resume. The input inherits the overlap of both
// histories and its first min(old in-flight count, T) in-flight values; the
// rest of its window is fresh.
//
// A Sequential input that the predecessor does not have (a new or anonymous
// port, or every input of a fresh start) owns a window too, and all of it is
// fresh. Its transition has no predecessor, and its previous_* indices are
// meaningless.
struct RealtimePortStateTransition {
    bool has_predecessor = true;
    std::size_t previous_requirement_index = 0;
    std::size_t current_requirement_index = 0;
    std::size_t previous_realization_index = 0;
    std::size_t current_realization_index = 0;
    // Complete successor interval.
    std::int64_t target_begin = 0;
    std::int64_t target_end = 0;
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
    SampleRealtimePortStateStorageView target_storage{};
    // One value of the target's geometry. Every position of the complete
    // successor interval is set to it before the inherited sub-interval is
    // transferred from predecessor storage: the value the input's storage
    // starts with when it declares one (a detach() initial value), otherwise
    // the successor input's neutral_value, and zero for a Tick output.
    std::vector<std::byte> fresh_value{};
    // Complete successor interval.
    std::int64_t target_begin = 0;
    std::int64_t target_end = 0;
    std::int64_t inherited_begin = 0;
    std::int64_t inherited_end = 0;
    // Set when the transfer fills a source suffix: transition-only storage that
    // stands in for this source of a composed input
    // (SampleRealtimePortStateRealization::composition_sources) while its state
    // stays in steady storage. The source was rewired, so its window holds the
    // old source's values, and the transition root's composition reads the
    // suffix, which a mirror then fills from the new source.
    std::optional<std::size_t> composition_source{};
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
    EventRealtimePortStateStorageView target_storage{};
    // Populated when several semantic output states share one indexed target
    // aggregate. Every inserted event receives this stable producer index.
    std::optional<std::size_t> target_index{};
    // Successor interval initialized by this descriptor, in semantic positions
    // relative to the cutover. Descriptors which share a physical target
    // contribute the union of these intervals.
    std::int64_t target_begin = 0;
    std::int64_t target_end = 0;
    std::int64_t inherited_begin = 0;
    std::int64_t inherited_end = 0;
    // The stored timestamp of an event at semantic position P is P plus the
    // view's timeline_offset (EventRealtimePortStateStorageView), in the
    // predecessor's source storage and the successor's target storage. A
    // transfer selects by the source offset and restamps by the difference.
    std::int64_t source_timeline_offset = 0;
    std::int64_t target_timeline_offset = 0;
    // Cold preparation replaces this with the earliest stored timestamp,
    // relative to the cutover, of every transfer that appends into the same
    // physical target. The realtime merge uses that shared origin so every
    // transfer has one chronological ordering across the cutover boundary.
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

// One 8-byte transition-only cell that receives the value of one Sequential
// pace cursor when the cutover's transfers run: the sequential index of the
// successor's first block at that pace. A transition-only input's copy from its
// steady representation reads it to skip the frames its window already holds.
struct SampleRealtimePortStateCutoverIndex {
    std::size_t sequential_cursor_node_storage_offset = 0;
    std::size_t node_storage_offset = 0;
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
    std::vector<SampleRealtimePortStateCutoverIndex> sample_cutover_indices{};
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

// The value every frame of one persistent sample region starts with.
struct FreshSampleStorageValue {
    std::size_t node_storage_offset = 0;
    std::vector<std::byte> value{};
};

// The fresh values of a graph that starts without a predecessor. Every
// Sequential input's window [-history, in_flight) then holds its fresh value
// (sequential_port_storage_planning.md). A reader reads a frame of its source's
// storage before that source writes it only inside its own window, so when every
// state with a window in one region agrees on one fresh value, initializing the
// whole region with it is the same as initializing each window. The result names
// each such region whose value is not zero bytes. A Tick output's own storage
// starts at zero. Where two states need different fresh values in one region,
// that region starts at zero, and the first generation is compiled as a cutover
// from the empty graph, whose transition-only storage gives the other state its
// fresh window (CompiledGraphCutover::from_empty_graph).
[[nodiscard]] std::expected<std::vector<FreshSampleStorageValue>, std::string>
plan_fresh_sample_storage_values(
    RealtimePortStateRequirements const& requirements,
    RealtimePortStateRealizations const& realizations);

// Realizes sample transition state whose successor steady binding cannot
// directly receive inherited values. Each source and target retains its own
// generation-owned Sequential pace cursor, so cutover remains independent of
// node-visible transport jumps and supports a change of resolved dyadic pace.
// Disjoint roles/channels of one
// authored port may share a private suffix ring while all steady offsets stay
// fixed. A predecessor input produced by one retained linear channel
// materialization can be reconstructed during the prepared cutover. Direct,
// materialized, and composed successor callback views can then be mirrored into
// the suffix by transition lowering. A rewired source of a composed input that
// stays in steady storage gets its own source suffix
// (SampleRealtimePortStateTransfer::composition_source), and a transition-only
// event state gets a suffix ring, empty for a window that inherits nothing.
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
