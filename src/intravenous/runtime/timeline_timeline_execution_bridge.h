#pragma once

#include <intravenous/bridge.h>

namespace iv {
class Timeline;
class TimelineExecution;

IV_DECLARE_BRIDGE(timeline_timeline_execution_bridge, Timeline, TimelineExecution);
}
