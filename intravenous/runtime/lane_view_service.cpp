#include "runtime/lane_view_service.h"

#include <stdexcept>
#include <utility>

namespace iv {
    LaneViewService::LaneViewService(LaneViewQueryProvider query_provider) :
        _query_provider(std::move(query_provider))
    {}

    void LaneViewService::set_query_provider(LaneViewQueryProvider query_provider)
    {
        _query_provider = std::move(query_provider);
    }

    void LaneViewService::set_update_sink(LaneViewUpdateSink update_sink)
    {
        _update_sink = std::move(update_sink);
    }

    LaneQueryResult LaneViewService::query_lanes(
        LaneQueryFilter const& filter,
        std::optional<size_t> start_index,
        std::optional<size_t> visible_lane_count
    ) const
    {
        if (!_query_provider) {
            throw std::runtime_error("lane view service has no query provider");
        }
        return _query_provider(filter, start_index, visible_lane_count);
    }

    std::unordered_set<uint64_t> LaneViewService::visible_lane_ids(LaneQueryResult const& result)
    {
        std::unordered_set<uint64_t> ids;
        ids.reserve(result.lanes.size());
        for (auto const& lane : result.lanes) {
            ids.insert(lane.lane_id);
        }
        return ids;
    }

    LaneViewResult LaneViewService::open_view(LaneViewRequest request)
    {
        if (request.view_id.empty()) {
            throw std::runtime_error("lane view id must not be empty");
        }

        auto lanes = query_lanes(
            request.query.filter,
            request.start_index,
            request.visible_lane_count
        );
        _active_views[request.view_id] = ActiveView {
            .filter = request.query.filter,
            .start_index = lanes.start_index,
            .visible_lane_count = lanes.visible_lane_count,
            .visible_lane_ids = visible_lane_ids(lanes),
        };
        return LaneViewResult {
            .view_id = std::move(request.view_id),
            .lanes = std::move(lanes),
        };
    }

    LaneViewResult LaneViewService::update_view(LaneViewRequest request)
    {
        if (request.view_id.empty()) {
            throw std::runtime_error("lane view id must not be empty");
        }

        auto lanes = query_lanes(
            request.query.filter,
            request.start_index,
            request.visible_lane_count
        );
        _active_views[request.view_id] = ActiveView {
            .filter = request.query.filter,
            .start_index = lanes.start_index,
            .visible_lane_count = lanes.visible_lane_count,
            .visible_lane_ids = visible_lane_ids(lanes),
        };
        return LaneViewResult {
            .view_id = std::move(request.view_id),
            .lanes = std::move(lanes),
        };
    }

    void LaneViewService::close_view(std::string const& view_id)
    {
        _active_views.erase(view_id);
    }

    void LaneViewService::mark_lane_set_changed()
    {
        for (auto const& [view_id, view] : _active_views) {
            (void)view;
            notify_view_changed(view_id);
        }
    }

    void LaneViewService::mark_lane_changed(LaneId lane_id)
    {
        if (!lane_id) {
            return;
        }

        for (auto const& [view_id, view] : _active_views) {
            if (view.visible_lane_ids.contains(lane_id.value)) {
                notify_view_changed(view_id);
            }
        }
    }

    void LaneViewService::notify_view_changed(std::string const& view_id)
    {
        auto view_it = _active_views.find(view_id);
        if (view_it == _active_views.end()) {
            return;
        }
        try {
            auto const& view = view_it->second;
            auto lanes = query_lanes(
                view.filter,
                view.start_index,
                view.visible_lane_count
            );
            view_it->second.visible_lane_ids = visible_lane_ids(lanes);
            if (_update_sink) {
                _update_sink(LaneViewResult {
                    .view_id = view_id,
                    .lanes = std::move(lanes),
                });
            }
        } catch (...) {
            // View refresh failures must not tear down the audio/runtime server.
        }
    }
}
