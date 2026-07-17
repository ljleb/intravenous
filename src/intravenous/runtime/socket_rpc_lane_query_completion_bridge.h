#pragma once

namespace iv {
class LaneQuerySchemaService;

void bind_socket_rpc_lane_query_completion_bridge(LaneQuerySchemaService &service);
void unbind_socket_rpc_lane_query_completion_bridge(LaneQuerySchemaService const &service);
} // namespace iv
