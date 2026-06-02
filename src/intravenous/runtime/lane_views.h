#pragma once

#include <intravenous/runtime/lane_filters_events.h>
#include <intravenous/runtime/lane_view_service.h>

#include <mutex>
#include <unordered_map>

namespace iv {
    class LaneViews {
        LaneViewService lane_views;
        mutable std::mutex mutex;
        std::unordered_map<std::string, LaneFilterResult> results_by_filter;

        void emit_updated(LaneViewResult update) const;
        LaneQueryResult query_lanes_from_snapshots(
            LaneQueryFilter const &filter,
            std::optional<size_t> start_index,
            std::optional<size_t> visible_lane_count) const;

    public:
        LaneViews();

        LaneViewResult open_view(LaneViewRequest request);
        LaneViewResult update_view(LaneViewRequest request);
        void close_view(std::string const &view_id);
        void handle_lane_filters_changed(LaneFiltersChanged const &change);
    };
} // namespace iv
