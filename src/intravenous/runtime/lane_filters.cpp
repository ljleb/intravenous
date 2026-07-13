#include <intravenous/runtime/lane_filters.h>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace iv {
namespace {
class FilterDatasetView final : public query::LaneQueryDataset {
    query::LaneQueryDataset const &base_;
    std::unordered_map<std::string, LaneFilters::RegisteredLaneFilter> const &filters_by_name_;

public:
    FilterDatasetView(
        query::LaneQueryDataset const &base,
        std::unordered_map<std::string, LaneFilters::RegisteredLaneFilter> const &filters_by_name)
        : base_(base)
        , filters_by_name_(filters_by_name)
    {}

    [[nodiscard]] query::LaneQuerySchema const &schema() const override
    {
        return base_.schema();
    }

    [[nodiscard]] size_t lane_count() const override
    {
        return base_.lane_count();
    }

    [[nodiscard]] std::uint64_t lane_id_at(size_t lane_index) const override
    {
        return base_.lane_id_at(lane_index);
    }

    [[nodiscard]] bool in_filter(size_t lane_index, std::string_view filter_name) const override
    {
        auto const it = filters_by_name_.find(std::string(filter_name));
        if (it == filters_by_name_.end()) {
            return false;
        }
        return it->second.matching_lane_ids.contains(base_.lane_id_at(lane_index));
    }

    [[nodiscard]] bool has_unit(size_t lane_index, query::LaneQueryPropertyId property) const override
    {
        return base_.has_unit(lane_index, property);
    }

    [[nodiscard]] std::optional<int> int_value(size_t lane_index, query::LaneQueryPropertyId property) const override
    {
        return base_.int_value(lane_index, property);
    }

    [[nodiscard]] std::optional<float> float_value(size_t lane_index, query::LaneQueryPropertyId property) const override
    {
        return base_.float_value(lane_index, property);
    }
};

LaneFilterResult filter_result_from(
    LaneFilters::RegisteredLaneFilter const &filter,
    std::function<LaneMetadata(LaneId)> const &metadata_for_lane,
    std::function<std::optional<std::string>(LaneId)> const &model_type_id_for_lane,
    std::function<InternedString(LaneId)> const &public_id_for_lane,
    std::function<std::vector<TimelineLaneOutputs>(std::vector<LaneId> const &)> const &outputs_for_lanes,
    std::function<void(std::vector<LaneId> const &, TimelineLaneVisitFn const &)> const &visit_lanes)
{
    if (filter.error_message.has_value()) {
        return LaneFilterResult{
            .filter_name = filter.name,
            .query_source = filter.query_source,
            .outcome = LaneFilterError{
                .filter_name = filter.name,
                .query_source = filter.query_source,
                .message = *filter.error_message,
            },
        };
    }
    return LaneFilterResult{
        .filter_name = filter.name,
        .query_source = filter.query_source,
        .outcome = FilteredLanesSnapshot{
            .filter_name = filter.name,
            .query_source = filter.query_source,
            .revision = filter.bound_ast.schema_revision,
            .lane_ids = filter.matching_lanes,
            .metadata_for_lane = metadata_for_lane,
            .model_type_id_for_lane = model_type_id_for_lane,
            .public_id_for_lane = public_id_for_lane,
            .outputs_for_lanes = outputs_for_lanes,
            .visit_lanes = visit_lanes,
        },
    };
}

void append_topological_filter_order(
    std::string const &filter_name,
    std::unordered_map<std::string, LaneFilters::RegisteredLaneFilter> const &filters_by_name,
    std::unordered_set<std::string> &temporary_marks,
    std::unordered_set<std::string> &permanent_marks,
    std::vector<std::string> &order)
{
    if (permanent_marks.contains(filter_name)) {
        return;
    }
    if (temporary_marks.contains(filter_name)) {
        return;
    }

    temporary_marks.insert(filter_name);
    if (auto const it = filters_by_name.find(filter_name); it != filters_by_name.end()) {
        for (auto const &dependency_name : it->second.dependencies) {
            if (!filters_by_name.contains(dependency_name)) {
                continue;
            }
            append_topological_filter_order(
                dependency_name,
                filters_by_name,
                temporary_marks,
                permanent_marks,
                order);
        }
    }
    temporary_marks.erase(filter_name);
    permanent_marks.insert(filter_name);
    order.push_back(filter_name);
}

std::vector<std::string> topological_filter_order(
    std::unordered_map<std::string, LaneFilters::RegisteredLaneFilter> const &filters_by_name)
{
    std::vector<std::string> order;
    order.reserve(filters_by_name.size());
    std::unordered_set<std::string> temporary_marks;
    std::unordered_set<std::string> permanent_marks;

    for (auto const &[filter_name, _] : filters_by_name) {
        append_topological_filter_order(
            filter_name,
            filters_by_name,
            temporary_marks,
            permanent_marks,
            order);
    }
    return order;
}
} // namespace

LaneFilters::RegisteredLaneFilter &LaneFilters::ensure_filter_locked(std::string const &filter_name)
{
    auto [it, inserted] = filters_by_name.try_emplace(filter_name);
    if (inserted) {
        it->second.name = filter_name;
    }
    if (it->second.bound_ast.root == nullptr && dataset) {
        rebind_filter_locked(it->second);
    }
    return it->second;
}

void LaneFilters::rebind_filter_locked(RegisteredLaneFilter &filter)
{
    if (!dataset) {
        return;
    }
    if (filter.parse_error_message.has_value()) {
        filter.bound_ast = {};
        filter.dependencies.clear();
        filter.error_message = filter.parse_error_message;
        filter.dirty = false;
        return;
    }
    if (filter.query_source.empty()) {
        filter.bound_ast = {};
        filter.dependencies.clear();
        filter.error_message.reset();
        // refresh_filter_locked materializes the all-lanes selection.
        filter.dirty = true;
        return;
    }
    filter.bound_ast = query::bind_lane_query_ast(filter.raw_ast, dataset->schema());
    filter.dependencies = query::collect_filter_references(filter.bound_ast);
    filter.error_message.reset();
    filter.dirty = true;
}

void LaneFilters::refresh_filter_locked(
    RegisteredLaneFilter &filter,
    std::unordered_set<std::string> &visiting)
{
    if (!dataset) {
        filter.matching_lanes.clear();
        filter.matching_lane_ids.clear();
        filter.error_message.reset();
        filter.dirty = false;
        return;
    }
    if (!filter.dirty) {
        return;
    }
    if (filter.parse_error_message.has_value()) {
        filter.matching_lanes.clear();
        filter.matching_lane_ids.clear();
        filter.error_message = filter.parse_error_message;
        filter.dirty = false;
        return;
    }
    // The empty query is the lanes-view's explicit all-lanes query.  Do not
    // hand an empty AST to the query executor: it has no predicate and is not
    // a useful representation of this intentional selection.
    if (filter.query_source.empty()) {
        filter.matching_lanes.clear();
        filter.matching_lane_ids.clear();
        filter.matching_lanes.reserve(dataset->lane_count());
        filter.matching_lane_ids.reserve(dataset->lane_count());
        for (size_t index = 0; index < dataset->lane_count(); ++index) {
            const auto lane = LaneId{dataset->lane_id_at(index)};
            filter.matching_lanes.push_back(lane);
            filter.matching_lane_ids.insert(lane.value);
        }
        filter.error_message.reset();
        filter.dirty = false;
        return;
    }
    if (visiting.contains(filter.name)) {
        filter.matching_lanes.clear();
        filter.matching_lane_ids.clear();
        filter.error_message = "cyclic lane filter dependency involving '" + filter.name + "'";
        filter.dirty = false;
        return;
    }

    visiting.insert(filter.name);
    try {
        if (filter.bound_ast.root == nullptr
            || filter.bound_ast.schema_revision != dataset->schema().revision()) {
            rebind_filter_locked(filter);
        }
        for (auto const &dependency_name : filter.dependencies) {
            auto const dependency_it = filters_by_name.find(dependency_name);
            if (dependency_it == filters_by_name.end()) {
                filter.matching_lanes.clear();
                filter.matching_lane_ids.clear();
                filter.error_message = "unknown referenced lane filter: " + dependency_name;
                filter.dirty = false;
                visiting.erase(filter.name);
                return;
            }
            refresh_filter_locked(dependency_it->second, visiting);
            if (dependency_it->second.error_message.has_value()) {
                filter.matching_lanes.clear();
                filter.matching_lane_ids.clear();
                filter.error_message =
                    "dependent lane filter '" + dependency_name + "' failed: "
                    + *dependency_it->second.error_message;
                filter.dirty = false;
                visiting.erase(filter.name);
                return;
            }
        }

        FilterDatasetView composed_dataset(*dataset, filters_by_name);
        auto const result = query::execute_lane_query(filter.bound_ast, composed_dataset);
        filter.matching_lanes.clear();
        filter.matching_lane_ids.clear();
        filter.matching_lanes.reserve(result.matching_lane_indexes.size());
        filter.matching_lane_ids.reserve(result.matching_lane_indexes.size());
        for (auto const lane_index : result.matching_lane_indexes) {
            auto const lane_id = LaneId{dataset->lane_id_at(lane_index)};
            filter.matching_lanes.push_back(lane_id);
            filter.matching_lane_ids.insert(lane_id.value);
        }
        filter.error_message.reset();
    } catch (std::exception const &ex) {
        filter.matching_lanes.clear();
        filter.matching_lane_ids.clear();
        filter.error_message = ex.what();
    }
    filter.dirty = false;
    visiting.erase(filter.name);
}

std::vector<LaneFilterResult> LaneFilters::refresh_all_filters_locked()
{
    std::vector<LaneFilterResult> results;
    if (!dataset) {
        return results;
    }

    // v1 execution model: any relevant change invalidates all stored filters.
    // Later we can narrow this to dependency- and property-directed invalidation.
    std::unordered_set<std::string> visiting;
    auto const order = topological_filter_order(filters_by_name);
    results.reserve(order.size());
    for (auto const &filter_name : order) {
        auto it = filters_by_name.find(filter_name);
        if (it == filters_by_name.end()) {
            continue;
        }
        refresh_filter_locked(it->second, visiting);
        results.push_back(filter_result_from(
            it->second,
            metadata_for_lane,
            model_type_id_for_lane,
            public_id_for_lane,
            outputs_for_lanes,
            visit_lanes));
    }
    return results;
}

void LaneFilters::store_filter(LaneFilterStoredRequest const &request)
{
    std::vector<LaneFilterResult> results;
    bool parse_failed = false;
    {
        std::scoped_lock lock(mutex);
        auto &filter = ensure_filter_locked(request.filter_name);
        if (filter.query_source != request.query_source) {
            filter.query_source = request.query_source;
            filter.raw_ast = {};
            filter.bound_ast = {};
            filter.dependencies.clear();
            filter.matching_lanes.clear();
            filter.matching_lane_ids.clear();
            filter.parse_error_message.reset();
            filter.error_message.reset();
            try {
                filter.raw_ast = parser.parse(filter.query_source);
            } catch (std::exception const &ex) {
                filter.raw_ast = {};
                filter.parse_error_message = ex.what();
                filter.error_message = filter.parse_error_message;
                filter.dirty = false;
                parse_failed = true;
            }
            if (!parse_failed) {
                filter.dirty = true;
            }
        }
        if (dataset) {
            for (auto &[_, stored_filter] : filters_by_name) {
                stored_filter.dirty = true;
            }
            results = refresh_all_filters_locked();
        }
    }
    if (!results.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_lane_filters_changed_event,
            LaneFiltersChanged{
                .all_filters_changed = true,
                .schema_change = {},
                .results = std::move(results),
            });
    }
}

void LaneFilters::remove_filter(std::string const &filter_name)
{
    std::vector<LaneFilterResult> results;
    {
        std::scoped_lock lock(mutex);
        filters_by_name.erase(filter_name);
        if (dataset) {
            for (auto &[_, stored_filter] : filters_by_name) {
                stored_filter.dirty = true;
            }
            results = refresh_all_filters_locked();
        }
    }
    if (!results.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_lane_filters_changed_event,
            LaneFiltersChanged{
                .all_filters_changed = true,
                .schema_change = {},
                .results = std::move(results),
            });
    }
}

void LaneFilters::handle_timeline_lanes_changed(TimelineLanesChanged const &change)
{
    LaneFiltersChanged notification;
    {
        std::scoped_lock lock(mutex);
        // Some structural notifications (notably visualization-sink churn)
        // describe a delta but do not carry a replacement query dataset.  A
        // null dataset means "keep the current snapshot", never "there are
        // no timeline lanes".
        if (change.dataset) {
            dataset = change.dataset;
            last_schema_change = change.schema_change;
        }
        if (change.metadata_for_lane) metadata_for_lane = change.metadata_for_lane;
        if (change.model_type_id_for_lane) model_type_id_for_lane = change.model_type_id_for_lane;
        if (change.public_id_for_lane) public_id_for_lane = change.public_id_for_lane;
        if (change.outputs_for_lanes) outputs_for_lanes = change.outputs_for_lanes;
        if (change.visit_lanes) visit_lanes = change.visit_lanes;

        for (auto &[_, filter] : filters_by_name) {
            if (change.schema_change.changed) {
                filter.bound_ast = {};
                filter.dependencies.clear();
            }
            filter.dirty = true;
        }
        notification.results = refresh_all_filters_locked();
        notification = LaneFiltersChanged{
            .all_filters_changed = true,
            .schema_change = last_schema_change,
            .results = std::move(notification.results),
        };
    }
    IV_INVOKE_LINKER_EVENT(iv_runtime_lane_filters_changed_event, notification);
}
} // namespace iv
