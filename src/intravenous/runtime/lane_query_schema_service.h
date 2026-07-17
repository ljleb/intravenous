#pragma once

#include <intravenous/query/lane_query_schema.h>

#include <mutex>

namespace iv {
class Timeline;

class LaneQuerySchemaService {
    mutable std::mutex mutex;
    query::LaneQuerySchema schema_ {};

public:
    void initialize(query::LaneQuerySchema schema);
    void handle_timeline_lanes_changed(Timeline &timeline);

    [[nodiscard]] query::LaneQuerySchema snapshot() const;
};
} // namespace iv
