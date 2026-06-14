#pragma once

namespace iv {
class TimelineExecution;

void bind_timeline_execution_lanes_visualization_bridge(TimelineExecution &execution);
void unbind_timeline_execution_lanes_visualization_bridge(TimelineExecution const &execution);
} // namespace iv
