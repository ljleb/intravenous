#pragma once

#include <cstdint>

namespace iv {

// A Tick node's replay declaration, `static constexpr TickReplayMode
// replay_mode`. Any mode except `disabled` makes the node intrinsically
// replayable. It also selects the output coverage that the compiler generates
// when the node authors no forward propagation. Tick still computes an ordinary
// sample at every position, from its resolved Sequential inputs; the output's
// IndexCoverage decides which of those samples are output, in realtime and in
// replay alike. An authored propagate_forward_coverage() takes precedence.
// Replayable nodes declare only ephemeral Tick outputs. They may consume a
// different node's persisted output through its selected recorded-data view.
enum class TickReplayMode : std::uint8_t {
    // Not intrinsically replayable.
    disabled,
    // Each output covers the union of the node's input coverages: it exists
    // only where at least one upstream contribution exists.
    input_union,
    // Each output covers the whole circular index domain, including where no
    // upstream contribution exists and for nodes without inputs.
    full,
};

} // namespace iv
