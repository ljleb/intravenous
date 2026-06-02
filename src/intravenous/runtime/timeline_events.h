#pragma once

#include <intravenous/basic_lane_nodes/type_erased.h>
#include <intravenous/lane_node/graph.h>
#include <intravenous/linker_event.h>
#include <intravenous/query/lane_query_dataset.h>
#include <intravenous/query/lane_query_schema.h>

#include <functional>
#include <memory>
#include <vector>

namespace iv {
    struct TimelineLaneUpsert {
        LaneId lane {};
        std::function<TypeErasedLaneNode()> make_node {};
        LaneMetadata metadata {};
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
