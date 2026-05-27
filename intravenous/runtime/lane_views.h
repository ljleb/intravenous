#pragma once

#include "runtime/lane_view_service.h"
#include "runtime/timeline_events.h"

namespace iv {
    class LaneViews {
        LaneViewService lane_views;

        void emit_updated(LaneViewResult update) const;

    public:
        LaneViews();

        LaneViewResult open_view(LaneViewRequest request);
        LaneViewResult update_view(LaneViewRequest request);
        void close_view(std::string const &view_id);
        void handle_timeline_lanes_changed(TimelineLanesChanged const &change);
    };
} // namespace iv
