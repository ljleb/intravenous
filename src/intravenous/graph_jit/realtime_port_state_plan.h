#pragma once

#include <intravenous/graph/port_ids.h>
#include <intravenous/graph_jit/stable_graph_identity.h>

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

} // namespace iv::graph_jit
