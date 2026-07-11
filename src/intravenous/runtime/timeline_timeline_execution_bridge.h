#pragma once

namespace iv {
class Timeline;
class TimelineExecution;

void bind_timeline_timeline_execution_bridge(Timeline &timeline, TimelineExecution &execution);
void unbind_timeline_timeline_execution_bridge(Timeline const &timeline, TimelineExecution const &execution);
}
