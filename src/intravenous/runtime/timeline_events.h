#pragma once

#include <intravenous/basic_lane_nodes/type_erased.h>
#include <intravenous/lane_node/graph.h>
#include <intravenous/linker_event.h>
#include <intravenous/query/lane_query_dataset.h>
#include <intravenous/query/lane_query_schema.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace iv {
    using TimelineLaneVisitFn = std::function<void(
        LaneId,
        TypeErasedLaneNode const&,
        LaneOutputConfig const&,
        std::vector<LaneInputConnection> const&,
        std::vector<std::string> const&)>;

    struct TimelineLaneUpsert {
        LaneId lane {};
        std::function<TypeErasedLaneNode()> make_node {};
        LaneMetadata metadata {};
        // Extra task-graph dependencies for this lane's task beyond its lane inputs.
        // Used by output lanes to depend_on the producing DSP task (the inverse of the
        // input path), carried on the lane definition so TimelineExecution learns it
        // through the single lane-creation path (no separate dependency event).
        std::vector<std::string> external_task_dependencies {};
    };

    struct TimelineLaneHierarchyUpdate {
        LaneId parent {};
        LaneId child {};
    };

    struct TimelineLaneOutputs {
        LaneId lane {};
        std::vector<LaneOutputConnection> outputs {};
    };

    struct TimelineLaneBatchUpdate {
        std::vector<TimelineLaneUpsert> upserts {};
        std::vector<LaneId> removals {};
        std::vector<LaneGraphConnection> connections_to_remove {};
        std::vector<LaneGraphConnection> connections_to_add {};
        std::vector<TimelineLaneHierarchyUpdate> hierarchy_removals {};
        std::vector<TimelineLaneHierarchyUpdate> hierarchy_additions {};
    };

    struct TimelineLanesChanged {
        bool lane_set_changed = false;
        query::LaneQueryDatasetPtr dataset {};
        query::LaneQuerySchemaChange schema_change {};
        std::function<LaneMetadata(LaneId)> metadata_for_lane {};
        std::function<std::vector<TimelineLaneOutputs>(std::vector<LaneId> const &)> outputs_for_lanes {};
        std::function<void(std::vector<LaneId> const &, TimelineLaneVisitFn const &)> visit_lanes {};
        std::vector<LaneId> created_lanes {};
        std::vector<LaneId> removed_lanes {};
        std::vector<LaneId> changed_lanes {};
    };

    using TimelineLanesChangedEvent =
        void (*)(TimelineLanesChanged const &);

    IV_DECLARE_LINKER_EVENT(
        TimelineLanesChangedEvent,
        iv_runtime_timeline_lanes_changed_event);
} // namespace iv
