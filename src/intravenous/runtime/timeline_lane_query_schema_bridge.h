#pragma once

#include <intravenous/bridge.h>

namespace iv {
class LaneQuerySchemaService;
class Timeline;

IV_DECLARE_BRIDGE(timeline_lane_query_schema_bridge, Timeline, LaneQuerySchemaService);
} // namespace iv
