#include "runtime/lane_views.h"

#include "runtime/lane_views_events.h"

namespace iv {
    LaneViews::LaneViews()
    {
        lane_views.set_query_provider(
            [](LaneQueryFilter const &filter,
               std::optional<size_t> start_index,
               std::optional<size_t> visible_lane_count) {
                LaneViewsQueryResultBuilder builder;
                IV_INVOKE_LINKER_EVENT(
                    iv_runtime_lane_views_query_requested_event,
                    LaneViewsQueryRequest{
                        .filter = filter,
                        .start_index = start_index,
                        .visible_lane_count = visible_lane_count,
                    },
                    builder);
                return builder.build();
            });
        lane_views.set_update_sink([this](LaneViewResult const &update) {
            emit_updated(update);
        });
    }

    void LaneViews::emit_updated(LaneViewResult update) const
    {
        IV_INVOKE_LINKER_EVENT(iv_runtime_lane_views_updated_event, update);
    }

    LaneViewResult LaneViews::open_view(LaneViewRequest request)
    {
        return lane_views.open_view(std::move(request));
    }

    LaneViewResult LaneViews::update_view(LaneViewRequest request)
    {
        return lane_views.update_view(std::move(request));
    }

    void LaneViews::close_view(std::string const &view_id)
    {
        lane_views.close_view(view_id);
    }

    void LaneViews::handle_timeline_lanes_changed(
        TimelineLanesChanged const &change)
    {
        if (change.lane_set_changed) {
            lane_views.mark_lane_set_changed();
            return;
        }
        for (auto const lane : change.changed_lanes) {
            lane_views.mark_lane_changed(lane);
        }
    }
} // namespace iv
