#pragma once

#include <cstdint>

namespace iv {

// Intrinsic permission to replay a deterministic, stateless Tick computation.
// Values computed at an executed position and logical output coverage are
// distinct. Acyclic input_union outputs follow upstream coverage; Tick members
// of feedback SCCs have effective full live coverage without changing this mode.
// Replay additionally
// requires contextual input availability and an acyclic dependency path; it
// never crosses a stateful .detach() delay. Known gaps in replay inputs are
// resolved by their declared Sequential fallback.
// Replayable nodes author computed Sequential outputs. Recorded incoming data
// from other nodes remain valid replay dependencies.
enum class TickReplayMode : std::uint8_t {
    disabled,
    input_union,
    full,
};

} // namespace iv
