#include "runtime/lane_views_events.h"

#include <stdexcept>
#include <utility>

namespace iv {
    void LaneViewsQueryResultBuilder::succeed(LaneQueryResult value)
    {
        result = std::move(value);
    }

    LaneQueryResult LaneViewsQueryResultBuilder::build() const
    {
        if (!result.has_value()) {
            throw std::runtime_error("runtime lane views query result was not provided");
        }
        return *result;
    }

    IV_DEFINE_LINKER_EVENT(
        LaneViewsQueryRequestedEvent,
        iv_runtime_lane_views_query_requested_event);
    IV_DEFINE_LINKER_EVENT(
        LaneViewsUpdatedEvent,
        iv_runtime_lane_views_updated_event);
} // namespace iv
