#pragma once

#include <cstdint>

namespace iv {

// Intrinsic permission to replay a deterministic, stateless Tick computation.
// Live Sequential outputs always exist at executed positions. Replay additionally
// requires contextual input availability and an acyclic dependency path; it
// never crosses a stateful .detach() delay. Known gaps in replay inputs are
// resolved by their declared Sequential fallback.
// Replayable nodes author computed Sequential outputs. Recorded incoming data
// from other nodes remain valid replay dependencies.
enum class TickReplayMode : std::uint8_t {
    disabled,
    full,
};

} // namespace iv
