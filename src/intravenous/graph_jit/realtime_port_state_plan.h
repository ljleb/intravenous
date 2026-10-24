#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/continuous_value.h>
#include <intravenous/graph/port_ids.h>
#include <intravenous/graph_jit/stable_graph_identity.h>
#include <intravenous/sample.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iv::graph_jit {

// Distinguishes the independently owned temporal pieces of one realtime port.
// These roles are semantic migration identities; they do not describe whether
// the current realization uses a ring, compact carry, alias, or materialization.
enum class RealtimePortStateRole : std::uint8_t {
    sequential_input_history,
    tick_output_history,
    tick_output_latency,
};

// Stable identity of one conceptual node-owned realtime state piece. Sample
// ports have one identity per semantic channel. Event ports have one stream
// identity and therefore leave channel empty.
struct StableRealtimePortStateId {
    StableConcreteNodeId node{};
    PortDirection direction = PortDirection::input;
    PortKind kind = PortKind::sample;
    std::string port_name{};
    std::size_t port_index = 0;
    std::optional<std::size_t> channel{};
    RealtimePortStateRole role =
        RealtimePortStateRole::sequential_input_history;

    bool operator==(StableRealtimePortStateId const&) const = default;
};

// One non-empty state extent owned by a concrete port in this compiled graph.
// configured_port/direction identify the generation-local port even when the
// concrete node is anonymous. stable_identity is present only when continuity
// across graph revisions is meaningful. extent_samples is the exact history or
// authored-future distance, never a rounded storage capacity.
struct RealtimePortStateRequirement {
    NodeBundlePortId configured_port{};
    PortDirection direction = PortDirection::input;
    std::optional<std::size_t> channel{};
    RealtimePortStateRole role =
        RealtimePortStateRole::sequential_input_history;
    std::size_t extent_samples = 0;
    std::optional<StableRealtimePortStateId> stable_identity{};
};

struct RealtimePortStateRequirements {
    std::vector<RealtimePortStateRequirement> states{};
};

// Final physical form of one sample-state requirement in a compiled
// realization. callback_transient is intentionally explicit: those bytes are
// valid only while the generated root owns its invocation arena, so a graph
// transition cannot mistake them for cross-callback state. compact_carry and
// ring name canonical NodeStorage representations; immutable_constant owns no
// mutable bytes.
enum class SampleRealtimePortStateStorage : std::uint8_t {
    immutable_constant,
    callback_transient,
    compact_carry,
    ring,
};

enum class SampleRealtimePortStateAccess : std::uint8_t {
    // The callback reads one compiler-owned constant timeline; no mutable
    // cross-callback bytes exist.
    immutable_constant,
    // One retained source contains this semantic channel directly.
    direct,
    // The callback channel is reconstructed from retained_sources using the
    // recorded whole-layout conversion and selected timeline window.
    materialized,
    // The callback channel is produced by a multi-source composition whose
    // retained derivation has not yet been reduced to cold transition inputs.
    composed,
    // No recoverable cross-callback source is currently known.
    callback_only,
};

struct SampleRealtimePortStateStorageView {
    SampleRealtimePortStateStorage storage =
        SampleRealtimePortStateStorage::callback_transient;
    ChannelLayout channel_layout{};
    ContinuousValueStorageGeometry geometry{};
    std::int32_t absolute_pace_exponent = 0;
    // Offset of the generation-owned linear Sequential cursor for this
    // resolved pace. Physical sample storage is addressed relative to this
    // cursor, not from the node-visible GlobalIndex. Constants carry the
    // cursor metadata too, although reading their value does not require it.
    std::optional<std::size_t> sequential_cursor_node_storage_offset{};
    std::size_t representation_channel = 0;

    // Physical absolute-frame coordinate for semantic position P is
    // P + timeline_offset_frames. Sequential inputs therefore use a negative
    // read-latency/channel-delay offset. Callback-facing Tick output views use
    // their positive authored storage latency; retained Tick output views are
    // already indexed in semantic output coordinates and therefore use zero.
    std::int64_t timeline_offset_frames = 0;
    // Capacity of the representation's absolute-indexed working ring.
    std::size_t working_frame_capacity = 0;
    // Frames physically present in the selected immutable/callback/persistent
    // backing. A compact carry stores retained frames rather than the full
    // working-ring capacity.
    std::size_t storage_frame_count = 0;
    // Compact carry may retain already-authored frames at or after the next
    // invocation boundary. Zero for constants, callback-local storage, and
    // persistent rings.
    std::size_t carry_future_frames = 0;

    // Exactly one location form is populated according to storage.
    std::optional<std::size_t> node_storage_offset{};
    std::optional<std::size_t> callback_arena_offset{};
    std::size_t storage_size_bytes = 0;
    std::optional<std::vector<std::byte>> constant_value{};
};

struct SampleRealtimePortStateMaterialization {
    ChannelLayout source_layout{};
    ChannelLayout target_layout{};
    std::size_t retained_before = 0;
    std::size_t latest_read_latency = 0;
};

struct SampleRealtimePortStateCompositionSource {
    SampleRealtimePortStateStorageView storage{};
    float coefficient = 0.0f;
};

struct SampleRealtimePortStateRealization {
    // Index into RealtimePortStateRequirements::states. Only sample
    // requirements have entries here; event realizations are planned
    // independently because event fan-in has different ownership semantics.
    std::size_t requirement_index = 0;
    // Exact channel view bound to the authored node callback.
    SampleRealtimePortStateStorageView authored_storage{};
    SampleRealtimePortStateAccess retained_access =
        SampleRealtimePortStateAccess::callback_only;
    // One entry for direct access, or every source-layout channel required to
    // reconstruct a materialized target channel. Entries name cross-callback
    // storage only; callback arena bytes are never exposed as retained state.
    std::vector<SampleRealtimePortStateStorageView> retained_sources{};
    std::optional<SampleRealtimePortStateMaterialization> materialization{};
    // Flattened linear sources for one composed semantic target channel.
    // Each view already includes its source read latency in timeline_offset.
    std::vector<SampleRealtimePortStateCompositionSource>
        composition_sources{};
};

enum class EventRealtimePortStateStorage : std::uint8_t {
    callback_transient,
    compact_carry,
    ring,
    // Compiler-emitted read-only zero-count sequence. No location is
    // populated.
    immutable_empty,
};

// Describes how one semantic event-state requirement can be recovered after
// the callback which authored or consumed it has returned.
enum class EventRealtimePortStateAccess : std::uint8_t {
    // A disconnected zero-capacity input is authoritatively empty and needs no
    // mutable retained representation.
    immutable_empty,
    // retained_storage contains exactly this semantic stream.
    direct,
    // retained_storage is the source of the recorded conversion/window below.
    materialized,
    // retained_storage is a merged stream with a source-index sidecar; select
    // retained_source_index to recover this producer's events.
    indexed_merged_source,
    // Retained events from several producers are merged without source
    // identity. This is a truthful non-recoverable classification which forces
    // transition planning to add producer-owned retention before cutover.
    shared_merged_source,
    // Only callback-local bytes currently represent the state. Transition
    // planning must trace/add retention rather than reading expired arena data.
    callback_only,
};

struct EventRealtimePortStateStorageView {
    EventRealtimePortStateStorage storage =
        EventRealtimePortStateStorage::callback_transient;
    EventTypeId type = EventTypeId::empty;
    std::size_t event_capacity = 0;
    std::size_t size_bytes = 0;
    std::size_t alignment = 1;

    std::size_t count_relative_offset = 0;
    std::size_t read_index_relative_offset = 0;
    std::size_t write_index_relative_offset = 0;
    bool has_sequential_timeline = false;
    std::size_t timeline_initialized_relative_offset = 0;
    std::size_t expected_next_time_relative_offset = 0;
    std::size_t events_relative_offset = 0;
    bool has_source_indices = false;
    std::size_t source_indices_relative_offset = 0;

    // Persistent storage populates node_storage_offset, callback_transient
    // populates callback_arena_offset, and immutable_empty populates neither.
    std::optional<std::size_t> node_storage_offset{};
    std::optional<std::size_t> callback_arena_offset{};
};

// One Tick output-latency state whose future interval is present in a retained
// event input representation. requirement_index names the output-latency
// requirement in the same compiled graph. conversion is the exact path from
// that output's raw type to the retained input type. target_source_index is
// present only when the retained representation owns a source-index sidecar.
struct EventRealtimePortStateFutureSource {
    std::size_t requirement_index = 0;
    EventConversionPlan conversion{};
    std::optional<std::size_t> target_source_index{};
};

struct EventRealtimePortStateRealization {
    std::size_t requirement_index = 0;
    // Exact representation bound to the authored node callback.
    EventRealtimePortStateStorageView authored_storage{};
    EventRealtimePortStateAccess retained_access =
        EventRealtimePortStateAccess::callback_only;
    // Cross-callback representation, when retained_access is not
    // immutable_empty or callback_only.
    std::optional<EventRealtimePortStateStorageView> retained_storage{};

    // Populated for indexed_merged_source and shared_merged_source. In the
    // latter case it records semantic ordering but cannot filter the stream.
    std::optional<std::size_t> retained_source_index{};

    // Populated for materialized input state. TimedEvent timestamps remain
    // absolute, so no separate timeline offset is required.
    std::optional<EventConversionPlan> materialization_conversion{};
    std::size_t materialization_history_samples = 0;
    bool materialization_selects_invocation_window = false;

    // Input history and source-authored future events can occupy one retained
    // producer-group representation. Transition planning uses these records to
    // reconstruct its future interval from the independently owned output
    // latency states without changing steady execution.
    std::vector<EventRealtimePortStateFutureSource> retained_future_sources{};
};

struct RealtimePortStateRealizations {
    std::vector<SampleRealtimePortStateRealization> sample_states{};
    std::vector<EventRealtimePortStateRealization> event_states{};
};

} // namespace iv::graph_jit
