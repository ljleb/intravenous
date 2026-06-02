#pragma once

#include <intravenous/lane_node/graph.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/runtime/timeline_fwd.h>

#include <string>

namespace iv {
class RealtimeLaneRef {
    Timeline* _timeline = nullptr;
    LaneId _lane {};

public:
    RealtimeLaneRef() = default;
    RealtimeLaneRef(Timeline& timeline, LaneId lane);

    explicit operator bool() const noexcept
    {
        return _timeline != nullptr && static_cast<bool>(_lane);
    }

    LaneId lane_id() const noexcept
    {
        return _lane;
    }

    std::string nominal_identity() const;

    RealtimeSampleBlockLease pull_sample_block(
        size_t start_index,
        size_t count) const;
};
} // namespace iv
