#include <intravenous/runtime/lane_views.h>

#include <intravenous/runtime/lane_views_events.h>

#include <algorithm>
#include <unordered_set>

namespace iv {
    namespace {
        std::string lane_view_filter_name(std::string const &view_id)
        {
            return "lane_view." + view_id;
        }
    }

    LaneViews::LaneViews()
    {
        lane_views.set_query_provider(
            [this](LaneQueryFilter const &filter,
                   std::optional<size_t> start_index,
                   std::optional<size_t> visible_lane_count) {
                return query_lanes_from_snapshots(
                    filter,
                    start_index,
                    visible_lane_count);
            });
        lane_views.set_update_sink([this](LaneViewResult const &update) {
            emit_updated(update);
        });
    }

    void LaneViews::emit_updated(LaneViewResult update) const
    {
        IV_INVOKE_LINKER_EVENT(iv_runtime_lane_views_updated_event, update);
    }

    LaneQueryResult LaneViews::query_lanes_from_snapshots(
        LaneQueryFilter const &filter,
        std::optional<size_t> start_index,
        std::optional<size_t> visible_lane_count) const
    {
        std::scoped_lock lock(mutex);
        auto const it = results_by_filter.find(filter.source);
        if (it == results_by_filter.end()) {
            return {};
        }

        auto const &filter_result = it->second;
        auto const *snapshot = std::get_if<FilteredLanesSnapshot>(&filter_result.outcome);
        if (snapshot == nullptr) {
            auto const &error = std::get<LaneFilterError>(filter_result.outcome);
            return LaneQueryResult{
                .error_message = error.message,
            };
        }
        LaneQueryResult result;
        result.total_lane_count = snapshot->lane_ids.size();
        result.start_index = start_index.value_or(0);
        result.visible_lane_count = visible_lane_count.value_or(result.total_lane_count);

        size_t const max_start_index =
            result.total_lane_count == 0
                ? 0
                : (result.visible_lane_count >= result.total_lane_count
                       ? 0
                       : result.total_lane_count - result.visible_lane_count);
        size_t const lane_begin = std::min(result.start_index, max_start_index);
        size_t const remaining_lanes = result.total_lane_count - lane_begin;
        size_t const window_lane_count = std::min(result.visible_lane_count, remaining_lanes);
        size_t const lane_end = lane_begin + window_lane_count;
        bool const restrict_connections_to_window =
            start_index.has_value() || visible_lane_count.has_value();
        result.start_index = lane_begin;
        result.lanes.reserve(window_lane_count);

        std::unordered_set<uint64_t> visible_lane_ids;
        visible_lane_ids.reserve(window_lane_count);
        std::vector<LaneId> requested_output_lanes;
        requested_output_lanes.reserve(window_lane_count);

        for (size_t lane_index = lane_begin; lane_index < lane_end; ++lane_index) {
            auto const lane_id = snapshot->lane_ids[lane_index];
            visible_lane_ids.insert(lane_id.value);
            requested_output_lanes.push_back(lane_id);
            result.lanes.push_back(LaneInfo{
                .lane_id = lane_id.value,
                .domain = LaneDomain::realtime,
                .metadata = snapshot->metadata_for_lane ? snapshot->metadata_for_lane(lane_id) : LaneMetadata{},
            });
        }

        if (snapshot->outputs_for_lanes) {
            auto const output_groups = snapshot->outputs_for_lanes(
                restrict_connections_to_window ? requested_output_lanes : snapshot->lane_ids);
            for (auto const &group : output_groups) {
                for (auto const &output : group.outputs) {
                    if (restrict_connections_to_window
                        && !visible_lane_ids.contains(group.lane.value)
                        && !visible_lane_ids.contains(output.target.value)) {
                        continue;
                    }
                    result.connections.push_back(LaneConnectionInfo{
                        .source_lane_id = group.lane.value,
                        .target_lane_id = output.target.value,
                        .port_kind = output.input.kind,
                        .port_ordinal = output.input.ordinal,
                    });
                }
            }
        }

        return result;
    }

    LaneViewResult LaneViews::open_view(LaneViewRequest request)
    {
        auto const query_source = request.query.filter.source;
        auto const filter_name = lane_view_filter_name(request.view_id);
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_lane_filter_stored_event,
            LaneFilterStoredRequest{
                .filter_name = filter_name,
                .query_source = query_source,
            });
        request.query.filter.source = filter_name;
        return lane_views.open_view(std::move(request));
    }

    LaneViewResult LaneViews::update_view(LaneViewRequest request)
    {
        auto const query_source = request.query.filter.source;
        auto const filter_name = lane_view_filter_name(request.view_id);
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_lane_filter_stored_event,
            LaneFilterStoredRequest{
                .filter_name = filter_name,
                .query_source = query_source,
            });
        request.query.filter.source = filter_name;
        return lane_views.update_view(std::move(request));
    }

    void LaneViews::close_view(std::string const &view_id)
    {
        auto const filter_name = lane_view_filter_name(view_id);
        {
            std::scoped_lock lock(mutex);
            results_by_filter.erase(filter_name);
        }
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_lane_filter_removed_event,
            filter_name);
        lane_views.close_view(view_id);
    }

    void LaneViews::handle_lane_filters_changed(
        LaneFiltersChanged const &change)
    {
        {
            std::scoped_lock lock(mutex);
            for (auto const &result : change.results) {
                results_by_filter[result.filter_name] = result;
            }
        }
        if (change.all_filters_changed || change.schema_change.changed || !change.results.empty()) {
            lane_views.mark_lane_set_changed();
        }
    }
} // namespace iv
