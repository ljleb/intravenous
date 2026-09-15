#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/basic_lane_nodes/type_erased.h>
#include <intravenous/lane_node/graph.h>
#include <intravenous/runtime/uuid.h>
#include <intravenous/query/lane_query_schema.h>

#include <functional>
#include <memory>
#include <variant>
#include <vector>

namespace iv {

using LaneFilterLaneVisitFn = std::function<void(
    LaneId,
    std::shared_ptr<TypeErasedLaneNode const> const&,
    LaneOutputConfig const&,
    std::optional<ChannelTypeId>,
    std::vector<LaneInputConnection> const&,
    std::vector<std::string> const&)>;

struct LaneFilterLaneOutputs {
    LaneId lane{};
    std::vector<LaneOutputConnection> outputs{};
};

struct FilteredLanesSnapshot {
    std::string filter_name {};
    std::string query_source {};
    std::uint64_t revision = 0;
    std::vector<LaneId> lane_ids {};
    std::function<LaneMetadata(LaneId)> metadata_for_lane {};
    std::function<std::optional<std::string>(LaneId)> model_type_id_for_lane {};
    std::function<InternedString(LaneId)> public_id_for_lane {};
    std::function<std::vector<LaneFilterLaneOutputs>(std::vector<LaneId> const &)> outputs_for_lanes {};
    std::function<void(std::vector<LaneId> const &, LaneFilterLaneVisitFn const &)> visit_lanes {};
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
