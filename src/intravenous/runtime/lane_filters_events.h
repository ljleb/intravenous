#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/timeline_events.h>
#include <intravenous/runtime/uuid.h>
#include <intravenous/query/lane_query_schema.h>

#include <functional>
#include <variant>
#include <vector>

namespace iv {
struct FilteredLanesSnapshot {
    std::string filter_name {};
    std::string query_source {};
    std::uint64_t revision = 0;
    std::vector<LaneId> lane_ids {};
    std::function<LaneMetadata(LaneId)> metadata_for_lane {};
    std::function<InternedString(LaneId)> public_id_for_lane {};
    std::function<std::vector<TimelineLaneOutputs>(std::vector<LaneId> const &)> outputs_for_lanes {};
    std::function<void(std::vector<LaneId> const &, TimelineLaneVisitFn const &)> visit_lanes {};
};

struct LaneFilterError {
    std::string filter_name {};
    std::string query_source {};
    std::string message {};
};

using LaneFilterOutcome = std::variant<FilteredLanesSnapshot, LaneFilterError>;

struct LaneFilterResult {
    std::string filter_name {};
    std::string query_source {};
    LaneFilterOutcome outcome {};
};

struct LaneFiltersChanged {
    bool all_filters_changed = true;
    query::LaneQuerySchemaChange schema_change {};
    std::vector<LaneFilterResult> results {};
};

using LaneFiltersChangedEvent = void (*)(LaneFiltersChanged const &);
struct LaneFilterStoredRequest {
    std::string filter_name {};
    std::string query_source {};
};
using LaneFilterStoredEvent = void (*)(LaneFilterStoredRequest const &);
using LaneFilterRemovedEvent = void (*)(std::string const &);

IV_DECLARE_LINKER_EVENT(
    LaneFiltersChangedEvent,
    iv_runtime_lane_filters_changed_event);
IV_DECLARE_LINKER_EVENT(
    LaneFilterStoredEvent,
    iv_runtime_lane_filter_stored_event);
IV_DECLARE_LINKER_EVENT(
    LaneFilterRemovedEvent,
    iv_runtime_lane_filter_removed_event);
} // namespace iv
