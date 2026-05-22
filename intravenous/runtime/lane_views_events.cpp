#include "runtime/lane_views_events.h"

#include <stdexcept>
#include <utility>

namespace iv {
    void RuntimeLaneViewsQueryResultBuilder::succeed(LaneQueryResult value)
    {
        result = std::move(value);
    }

    LaneQueryResult RuntimeLaneViewsQueryResultBuilder::build() const
    {
        if (!result.has_value()) {
            throw std::runtime_error("runtime lane views query result was not provided");
        }
        return *result;
    }

    IV_DEFINE_LINKER_EVENT(
        RuntimeLaneViewsQueryRequestedEvent,
        iv_runtime_lane_views_query_requested_event);
    IV_DEFINE_LINKER_EVENT(
        RuntimeLaneViewsUpdatedEvent,
        iv_runtime_lane_views_updated_event);
} // namespace iv
