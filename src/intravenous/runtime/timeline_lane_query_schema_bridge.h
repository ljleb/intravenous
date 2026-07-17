#pragma once

namespace iv {
class LaneQuerySchemaService;
class Timeline;

void bind_timeline_lane_query_schema_bridge(
    LaneQuerySchemaService &service,
    Timeline &timeline);
void unbind_timeline_lane_query_schema_bridge(
    LaneQuerySchemaService const &service,
    Timeline const &timeline);
} // namespace iv
