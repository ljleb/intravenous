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

// Distinguishes the independently owned temporal pieces of one realtime port,
// whatever its stream kind (sequential_port_storage_planning.md, "Sample and
// event streams share one lifetime and ownership model"). These roles are
// semantic migration identities; they do not describe whether the current
// realization uses a ring, compact carry, alias, or materialization. Positions
// are relative to the port's next tick: a negative position is a value owned
// before it.
enum class RealtimePortStateRole : std::uint8_t {
    // Values a Sequential input owns at its read delay: its history
    // [-history, 0), then its in-flight values [0, in_flight), which its sources
    // have finalized but its read delay beyond their latency has not yet let it
    // read.
    sequential_input_history,
    // Values a Tick output has authored and still owns before its next tick:
    // [-extent, 0), its last history + latency positions. Storage files each
    // value or event at the position it was authored for; latency only marks
    // the newest positions as still open to authoring.
    tick_output_history,
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
// across graph revisions is meaningful. extent_samples is the exact owned extent
// of the role (see RealtimePortStateRole), never a rounded storage capacity.
// For a Sequential input, extent_samples is its history and in_flight_samples
// its read delay beyond its source's latency (a sample channel's, or the
// largest of an event stream's deliveries): compiler-owned compensation plus
// any detach() delay. The record owns [-extent_samples, in_flight_samples).
struct RealtimePortStateRequirement {
    NodeBundlePortId configured_port{};
    PortDirection direction = PortDirection::input;
    std::optional<std::size_t> channel{};
    RealtimePortStateRole role =
        RealtimePortStateRole::sequential_input_history;
    std::size_t extent_samples = 0;
    std::size_t in_flight_samples = 0;
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
    // Stable identity of the Tick output whose timeline this view addresses,
    // and that output's channel, when the view is that output's canonical
    // producer representation. A cutover compares them to tell whether a
    // predecessor view and a successor view hold the same producer's values.
    std::optional<StableOutputPortId> producer{};
    std::size_t producer_channel = 0;
    // That Tick output's resolved latency, when known. A reader's in-flight
    // values from it are then its read delay minus this latency.
    std::optional<std::size_t> producer_latency{};
    // True when the view is a detach() feedback timeline
    // (SampleFeedbackTimelinePlan) written by a copy or a composition. Only
    // its own connection's input reads that storage, so a cutover may place
    // that input's values in it whatever produced them.
    bool feedback_timeline = false;
    // Offset of the generation-owned linear Sequential cursor for this
    // resolved pace. Physical sample storage is addressed relative to this
    // cursor, not from the node-visible GlobalIndex. Constants carry the
    // cursor metadata too, although reading their value does not require it.
    std::optional<std::size_t> sequential_cursor_node_storage_offset{};
    std::size_t representation_channel = 0;

    // Physical absolute-frame index for semantic position P is
    // P + timeline_offset_frames. Sequential inputs use a negative
    // read-latency/channel-delay offset. Tick output storage files each value at
    // the index it was authored for, so every Tick output view uses zero.
    std::int64_t timeline_offset_frames = 0;
    // Capacity of the representation's absolute-indexed working ring.
    std::size_t working_frame_capacity = 0;
    // Frames physically present in the selected immutable/callback/persistent
    // backing. A compact carry stores retained frames rather than the full
    // working-ring capacity.
    std::size_t storage_frame_count = 0;
    // Retained frames at or after the next root invocation's first index. Only
    // shifted composed feedback carries any; zero for Tick output storage,
    // constants, callback-local storage, and persistent rings.
    std::size_t carry_future_frames = 0;

    // Exactly one location form is populated according to storage.
    std::optional<std::size_t> node_storage_offset{};
    std::optional<std::size_t> callback_arena_offset{};
    std::size_t storage_size_bytes = 0;
    std::optional<std::vector<std::byte>> constant_value{};
    // The value of every frame of this storage before anything is written to
    // it, when its persistent allocation declares one: a feedback timeline's
    // detach() initial value, a slot-fed input timeline's neutral value, or
    // the fresh value on which every state with a window in producer storage
    // agrees (plan_fresh_sample_storage_values()). Other producer storage
    // starts at zero bytes and declares none.
    std::optional<std::vector<std::byte>> initial_value{};
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
    // One value of a Sequential input's geometry: its neutral_value. Empty for
    // an output.
    std::vector<std::byte> neutral_value{};
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

    // The stored timestamp of an event at semantic position P of the state is
    // P + timeline_offset. A latent Tick output's own retained stream holds
    // each event where it becomes final, so its views use the output's
    // latency. A Sequential input's producer-group stream and a detach()
    // delayed stream hold each event at the position the input reads it, so
    // their views use zero.
    std::int64_t timeline_offset = 0;
};

// How a Sequential event input reads one of its sources: the Tick output it
// reads, when that output has a stable identity; its read delay behind the
// output's own positions (the output's latency, compensation, and any detach()
// delay); the output's latency; and whether it reads a detach() delayed stream
// that only its connection's inputs read. Transition planning uses these as it
// uses a sample view's producer, timeline offset and producer latency.
struct EventRealtimePortStateSourceTiming {
    std::optional<StableOutputPortId> producer{};
    std::size_t read_delay = 0;
    std::optional<std::size_t> producer_latency{};
    bool feedback_timeline = false;
    // The source's semantic position in its connection: its index in a
    // fan-in stream's source-index sidecar.
    std::size_t source_index = 0;
};

// One Tick event output whose events a retained input stream (a producer
// group's fan-in or conversion stream) also holds. Besides the input's own
// window, that stream holds the output's latency positions, which are not
// final yet and belong to the output. requirement_index names the output's
// requirement in the same compiled graph; conversion is the path from the
// output's raw type to the stream's type; latency is the output's; the stream
// holds each of its events stream_delay positions after its own position (the
// output's latency plus the readers' compensation); target_source_index is
// present only when the stream owns a source-index sidecar.
struct EventRealtimePortStateOutputSource {
    std::size_t requirement_index = 0;
    EventConversionPlan conversion{};
    std::size_t latency = 0;
    std::size_t stream_delay = 0;
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

    // One entry per realtime delivery a Sequential input receives. Empty for an
    // output.
    std::vector<EventRealtimePortStateSourceTiming> source_timings{};
    // The outputs whose latency positions the input's retained stream also
    // holds. A cutover seeds them there from each output's own state.
    std::vector<EventRealtimePortStateOutputSource> retained_output_sources{};
};

struct RealtimePortStateRealizations {
    std::vector<SampleRealtimePortStateRealization> sample_states{};
    std::vector<EventRealtimePortStateRealization> event_states{};
};

} // namespace iv::graph_jit
