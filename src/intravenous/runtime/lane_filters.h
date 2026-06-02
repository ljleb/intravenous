#pragma once

#include "query/lane_query_engine.h"
#include "query/lane_query_parser.h"
#include "query/lane_query_program.h"
#include "runtime/lane_filters_events.h"
#include "runtime/lane_view_service.h"
#include "runtime/timeline_events.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
class LaneFilters {
public:
    struct RegisteredLaneFilter {
        std::string name {};
        std::string query_source {};
        query::RawLaneQueryAst raw_ast {};
        query::BoundLaneQueryAst bound_ast {};
        std::vector<std::string> dependencies {};
        std::vector<LaneId> matching_lanes {};
        std::unordered_set<std::uint64_t> matching_lane_ids {};
        std::optional<std::string> parse_error_message {};
        std::optional<std::string> error_message {};
        bool dirty = true;
    };

private:
    mutable std::mutex mutex;
    query::LaneQueryDatasetPtr dataset;
    query::LaneQuerySchemaChange last_schema_change {};
    std::function<LaneMetadata(LaneId)> metadata_for_lane;
    std::function<std::vector<TimelineLaneOutputs>(std::vector<LaneId> const &)> outputs_for_lanes;
    std::unordered_map<std::string, RegisteredLaneFilter> filters_by_name;
    query::LaneQueryParser parser;

    RegisteredLaneFilter &ensure_filter_locked(std::string const &filter_name);
    void rebind_filter_locked(RegisteredLaneFilter &filter);
    void refresh_filter_locked(
        RegisteredLaneFilter &filter,
        std::unordered_set<std::string> &visiting);
    [[nodiscard]] std::vector<LaneFilterResult> refresh_all_filters_locked();

public:
    LaneFilters() = default;

    void store_filter(LaneFilterStoredRequest const &request);
    void remove_filter(std::string const &filter_name);
    void handle_timeline_lanes_changed(TimelineLanesChanged const &change);
};
} // namespace iv
