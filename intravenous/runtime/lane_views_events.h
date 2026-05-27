#pragma once

#include "linker_event.h"
#include "runtime/lane_view_service.h"

#include <optional>

namespace iv {
    struct LaneViewsQueryRequest {
        LaneQueryFilter filter {};
        std::optional<size_t> start_index;
        std::optional<size_t> visible_lane_count;
    };

    class LaneViewsQueryResultBuilder {
        std::optional<LaneQueryResult> result;

    public:
        void succeed(LaneQueryResult value);
        [[nodiscard]] LaneQueryResult build() const;
    };

    using LaneViewsQueryRequestedEvent =
        void (*)(LaneViewsQueryRequest const &, LaneViewsQueryResultBuilder &);
    using LaneViewsUpdatedEvent =
        void (*)(LaneViewResult const &);

    IV_DECLARE_LINKER_EVENT(
        LaneViewsQueryRequestedEvent,
        iv_runtime_lane_views_query_requested_event);
    IV_DECLARE_LINKER_EVENT(
        LaneViewsUpdatedEvent,
        iv_runtime_lane_views_updated_event);
} // namespace iv
