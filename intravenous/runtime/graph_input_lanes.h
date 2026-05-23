#pragma once

#include "runtime/iv_module_instances.h"
#include "runtime/graph_input_lanes_events.h"
#include "runtime/lane_view_service.h"
#include "runtime/project_introspection_events.h"
#include "runtime/runtime_project_events.h"

#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iv {
class RuntimeGraphInputLanes {
    mutable std::mutex mutex;
    std::vector<GraphInputPortDescriptor> current_ports;

    static std::vector<GraphInputPortDescriptor> graph_input_port_descriptors_for(
        std::span<IntrospectionLogicalNode const> logical_nodes);
    LaneQueryResult query_lanes_locked(
        LaneQueryFilter const &filter,
        std::optional<size_t> start_index = std::nullopt,
        std::optional<size_t> visible_lane_count = std::nullopt);

public:
    RuntimeGraphInputLanes() = default;

    void handle_iv_module_instances_changed(
        RuntimeIvModuleInstancesChanged const &diff);
    std::vector<RuntimeProjectIntrospectionLiveInputSnapshot> collect_live_input_snapshots(
        std::vector<RuntimeProjectIntrospectionLiveInputSnapshotRequest> const &requests);
    void ensure_graph_input_lane_bindings(
        RuntimeProjectGraphInputLaneBindingsRequest const &request);
    GraphInputLaneBindings query_graph_input_lane_bindings(
        RuntimeProjectGraphInputLaneBindingsRequest const &request);
    std::vector<RuntimeProjectLaneOutputs> query_lane_outputs(
        RuntimeProjectLaneOutputsRequest const &request);
    void set_sample_input_value(
        RuntimeProjectSetSampleInputValueRequest const &request);
    void clear_sample_input_value_override(
        RuntimeProjectClearSampleInputValueOverrideRequest const &request);
    RealtimeLaneRef resolve_sample_input_lane_ref(
        RuntimeGraphInputLanesSampleInputLaneRefRequest const &request);
    LaneQueryResult query_lanes(
        LaneQueryFilter const &filter,
        std::optional<size_t> start_index = std::nullopt,
        std::optional<size_t> visible_lane_count = std::nullopt);
};
} // namespace iv
