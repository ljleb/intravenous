#include <intravenous/runtime/lane_views.h>

#include <intravenous/runtime/lane_views_events.h>

#include <algorithm>
#include <unordered_set>
#include <variant>

namespace iv {
    namespace {
        LaneDomain lane_domain(LaneOutputConfig const &output)
        {
            return std::holds_alternative<CompiledSampleLaneOutputConfig>(output)
                    || std::holds_alternative<CompiledEventLaneOutputConfig>(output)
                ? LaneDomain::compiled
                : LaneDomain::realtime;
        }

        std::string lane_view_filter_name(InternedString view_id)
        {
            return "lane_view." + view_id.str();
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
            auto const runtime_lane = snapshot->lane_ids[lane_index];
            visible_lane_ids.insert(runtime_lane.value);
            requested_output_lanes.push_back(runtime_lane);
            result.lanes.push_back(LaneInfo{
                .lane_id = snapshot->public_id_for_lane
                    ? snapshot->public_id_for_lane(runtime_lane)
                    : InternedString::from_string(std::to_string(runtime_lane.value)),
                .runtime_lane = runtime_lane,
                .domain = LaneDomain::realtime,
                .metadata = snapshot->metadata_for_lane ? snapshot->metadata_for_lane(runtime_lane) : LaneMetadata{},
                .model_type_id = snapshot->model_type_id_for_lane
                    ? snapshot->model_type_id_for_lane(runtime_lane)
                    : std::optional<std::string>{},
            });
        }

        if (snapshot->visit_lanes) {
            std::unordered_map<uint64_t, LaneDomain> domains;
            snapshot->visit_lanes(
                requested_output_lanes,
                [&domains](LaneId lane, TypeErasedLaneNode const &, LaneOutputConfig const &output,
                           std::optional<ChannelTypeId>, std::vector<LaneInputConnection> const &,
                           std::vector<std::string> const &) {
                    domains[lane.value] = lane_domain(output);
                });
            for (auto &lane : result.lanes) {
                if (auto const it = domains.find(lane.runtime_lane.value); it != domains.end()) {
                    lane.domain = it->second;
                }
            }
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
                        .source_lane_id = snapshot->public_id_for_lane
                            ? snapshot->public_id_for_lane(group.lane)
                            : InternedString::from_string(std::to_string(group.lane.value)),
                        .target_lane_id = snapshot->public_id_for_lane
                            ? snapshot->public_id_for_lane(output.target)
                            : InternedString::from_string(std::to_string(output.target.value)),
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
        auto result = lane_views.open_view(std::move(request));
        // Opening a view is itself a visibility change. Publish it immediately
        // so visualization starts tracking its lanes without waiting for a
        // later filter or graph mutation.
        emit_updated(result);
        return result;
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
        auto result = lane_views.update_view(std::move(request));
        // Viewport changes must update the visualization subscription even
        // when the underlying lane-filter result has not changed.
        emit_updated(result);
        return result;
    }

    void LaneViews::close_view(std::string const &view_id)
    {
        auto const interned_view_id = InternedString::from_string(view_id);
        auto const filter_name = lane_view_filter_name(interned_view_id);
        {
            std::scoped_lock lock(mutex);
            results_by_filter.erase(filter_name);
        }
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_lane_filter_removed_event,
            filter_name);
        lane_views.close_view(interned_view_id);
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_lane_view_closed_event,
            view_id);
    }

    std::vector<LaneViewRequest> LaneViews::active_view_requests() const
    {
        std::vector<LaneViewRequest> requests;
        auto active_views = lane_views.active_views();
        requests.reserve(active_views.size());

        std::scoped_lock lock(mutex);
        for (auto const &view : active_views) {
            auto query_source = view.filter.source;
            if (auto const it = results_by_filter.find(view.filter.source);
                it != results_by_filter.end()) {
                query_source = it->second.query_source;
            }
            requests.push_back(LaneViewRequest{
                .view_id = InternedString::from_string(view.view_id),
                .query = LaneQuery{
                    .filter = LaneQueryFilter{
                        .source = query_source,
                    },
                },
                .start_index = view.start_index,
                .visible_lane_count = view.visible_lane_count,
                .first_sample_index = view.first_sample_index,
                .last_sample_index = view.last_sample_index,
                .display_sample_count = view.display_sample_count,
            });
        }
        return requests;
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
