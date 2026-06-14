#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/lane_view_service.h>

namespace iv {
    using LaneViewsUpdatedEvent =
        void (*)(LaneViewResult const &);
    using LaneViewClosedEvent =
        void (*)(std::string const &);

    IV_DECLARE_LINKER_EVENT(
        LaneViewsUpdatedEvent,
        iv_runtime_lane_views_updated_event);
    IV_DECLARE_LINKER_EVENT(
        LaneViewClosedEvent,
        iv_runtime_lane_view_closed_event);
} // namespace iv
