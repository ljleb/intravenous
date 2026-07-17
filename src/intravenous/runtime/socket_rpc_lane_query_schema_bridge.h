#pragma once

namespace iv {
class LaneQuerySchemaService;

void bind_socket_rpc_lane_query_schema_bridge(LaneQuerySchemaService &service);
void unbind_socket_rpc_lane_query_schema_bridge(LaneQuerySchemaService const &service);
} // namespace iv
