#pragma once

#include <intravenous/channel_layout.h>
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

struct SampleRealtimePortStateRealization {
    // Index into RealtimePortStateRequirements::states. Only sample
    // requirements have entries here; event realizations are planned
    // independently because event fan-in has different ownership semantics.
    std::size_t requirement_index = 0;
    SampleRealtimePortStateStorage storage =
        SampleRealtimePortStateStorage::callback_transient;
    ChannelLayout channel_layout{};
    std::size_t representation_channel = 0;

    // Physical absolute-frame coordinate for semantic position P is
    // P + timeline_offset_frames. Sequential inputs therefore use a negative
    // read-latency/channel-delay offset; Tick outputs use their positive
    // authored storage latency.
    std::int64_t timeline_offset_frames = 0;
    // Capacity of the callback-facing absolute-indexed representation.
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
    std::optional<Sample> constant_value{};
};

struct RealtimePortStateRealizations {
    std::vector<SampleRealtimePortStateRealization> sample_states{};
};

} // namespace iv::graph_jit
