#pragma once

namespace iv {
class Timeline;

void bind_timeline_lane_batch_bridge(Timeline &timeline);
void unbind_timeline_lane_batch_bridge(Timeline const &timeline);
} // namespace iv
