#pragma once

#include <intravenous/query/lane_query_schema.h>

#include <mutex>

namespace iv {
class Timeline;
struct CompleteLaneQueryRequest;
struct GetLaneQuerySchemaRequest;
class SocketRpcLaneQueryCompletionResultBuilder;
class SocketRpcLaneQuerySchemaResultBuilder;

class LaneQuerySchemaService {
    mutable std::mutex mutex;
    query::LaneQuerySchema schema_ {};

public:
    void initialize(query::LaneQuerySchema schema);
    void handle_timeline_lanes_changed(Timeline &timeline);
    void handle_socket_rpc_get_lane_query_schema(
        GetLaneQuerySchemaRequest const &request,
        SocketRpcLaneQuerySchemaResultBuilder &builder) const;
    void handle_socket_rpc_complete_lane_query(
        CompleteLaneQueryRequest const &request,
        SocketRpcLaneQueryCompletionResultBuilder &builder) const;

    [[nodiscard]] query::LaneQuerySchema snapshot() const;
};
} // namespace iv
