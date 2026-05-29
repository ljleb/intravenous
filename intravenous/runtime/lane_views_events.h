#pragma once

#include "linker_event.h"
#include "runtime/lane_view_service.h"

namespace iv {
    using LaneViewsUpdatedEvent =
        void (*)(LaneViewResult const &);

    IV_DECLARE_LINKER_EVENT(
        LaneViewsUpdatedEvent,
        iv_runtime_lane_views_updated_event);
} // namespace iv
