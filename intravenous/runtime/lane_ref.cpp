#include "runtime/lane_ref.h"

#include "runtime/timeline.h"

namespace iv {
RealtimeLaneRef::RealtimeLaneRef(Timeline& timeline, LaneId lane) :
    _timeline(&timeline),
    _lane(lane)
{
}

std::string RealtimeLaneRef::nominal_identity() const
{
    return "timeline-input-lane:" + std::to_string(_lane.value);
}

RealtimeSampleBlockLease RealtimeLaneRef::pull_sample_block(
    size_t start_index,
    size_t count) const
{
    return _timeline->pull_realtime_lane_sample_block(_lane, start_index, count);
}
} // namespace iv
