#pragma once

#include "linker_event.h"
#include "runtime/lane_view_service.h"

#include <optional>

namespace iv {
    struct RuntimeLaneViewsQueryRequest {
        LaneQueryFilter filter {};
        std::optional<size_t> start_index;
        std::optional<size_t> visible_lane_count;
    };

    class RuntimeLaneViewsQueryResultBuilder {
        std::optional<LaneQueryResult> result;

    public:
        void succeed(LaneQueryResult value);
        [[nodiscard]] LaneQueryResult build() const;
    };

    using RuntimeLaneViewsQueryRequestedEvent =
        void (*)(RuntimeLaneViewsQueryRequest const &, RuntimeLaneViewsQueryResultBuilder &);
    using RuntimeLaneViewsUpdatedEvent =
        void (*)(LaneViewResult const &);

    IV_DECLARE_LINKER_EVENT(
        RuntimeLaneViewsQueryRequestedEvent,
        iv_runtime_lane_views_query_requested_event);
    IV_DECLARE_LINKER_EVENT(
        RuntimeLaneViewsUpdatedEvent,
        iv_runtime_lane_views_updated_event);
} // namespace iv
