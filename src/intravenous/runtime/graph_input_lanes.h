#pragma once

#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>
#include <intravenous/runtime/lane_ref.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/lane_graph.h>

#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
class GraphInputLanes {
public:
    struct DesiredGraphInputPort {
        std::string instance_id {};
        int module_instance_id = 0;
        GraphInputPortDescriptor port {};
    };

    struct ExistingTrackedLane {
        LaneId lane {};
        LaneMetadata metadata {};
    };

    struct CompletedSampleInput {
        GraphBuilder::VacantSampleInput input {};
        LaneId lane {};
    };

    struct CompletedEventInput {
        GraphBuilder::VacantEventInput input {};
        LaneId lane {};
    };

    struct BuilderCompletionDiff {
        TimelineLaneBatchUpdate timeline_batch {};
        std::vector<CompletedSampleInput> sample_inputs {};
        std::vector<CompletedEventInput> event_inputs {};
    };

private:
    mutable std::mutex mutex;
    LaneIdAllocator lane_ids;
    std::unordered_map<std::string, IvModuleInstanceBuilder> builders_by_instance_id;
    std::vector<DesiredGraphInputPort> desired_ports;
    std::vector<ExistingTrackedLane> tracked_lanes;
    std::function<RealtimeLaneRef(LaneId)> realtime_lane_ref_for_lane;
    std::unordered_map<std::string, std::vector<std::vector<Sample*>>> live_inputs;
    std::unordered_map<std::string, std::vector<Sample>> live_input_values;
    std::unordered_map<std::string, Sample> sample_input_default_values;
    std::unordered_set<std::string> concrete_live_input_overrides;

    static std::vector<DesiredGraphInputPort> graph_input_port_descriptors_for(
        IvModuleInstanceBuilder const &instance_builder);
    static int module_instance_numeric_id(std::string_view instance_id);
    static int hash_string(std::string const &value);
    static std::string concrete_key(std::string_view logical_node_id, size_t member_ordinal);
    static std::string concrete_key_prefix(std::string_view logical_node_id);
    static std::string concrete_override_key(
        std::string_view logical_node_id,
        size_t member_ordinal,
        size_t input_ordinal);
    static std::string desired_port_key(DesiredGraphInputPort const &port);
    static std::string graph_input_port_key(GraphInputPortDescriptor const &port);
    static std::string sample_default_value_key(
        std::string_view instance_id,
        GraphInputPortDescriptor const &port);
    static GraphInputPortDescriptor sample_input_descriptor(
        std::string const &node_id,
        std::optional<size_t> member_ordinal,
        size_t input_ordinal);
    static LaneMetadata graph_input_metadata(
        DesiredGraphInputPort const &port,
        bool knob,
        bool logical,
        bool concrete,
        bool sample,
        bool event);
    static bool lane_metadata_matches_port(
        LaneMetadata const &metadata,
        DesiredGraphInputPort const &port);
    static bool has_concrete_descriptor_for_port(
        std::span<DesiredGraphInputPort const> ports,
        DesiredGraphInputPort const &logical_port);
    std::vector<Sample*>& ensure_live_input_slots_locked(std::string_view key, size_t input_ordinal);
    Sample& ensure_live_input_value_locked(std::string_view key, size_t input_ordinal);
    Sample live_input_value_or_locked(std::string_view logical_node_id, size_t input_ordinal, Sample fallback) const;
    Sample live_input_value_or_locked(
        std::string_view logical_node_id,
        size_t member_ordinal,
        size_t input_ordinal,
        Sample fallback) const;
    GraphInputLaneBindings reconcile_ports_locked(TimelineLaneBatchUpdate *batch = nullptr);
    void refresh_desired_ports_locked();
    GraphInputLaneBindings sample_input_bindings(
        std::string const &node_id,
        std::optional<size_t> member_ordinal,
        size_t input_ordinal);
    GraphInputLaneBindings query_graph_input_lane_bindings(
        ProjectGraphInputLaneBindingsRequest const &request);
    std::optional<LaneMetadata> tracked_lane_metadata_locked(LaneId lane) const;
    void apply_tracked_batch_locked(TimelineLaneBatchUpdate const &batch);
    void apply_timeline_batch(TimelineLaneBatchUpdate const &batch);

public:
    GraphInputLanes() = default;

    void set_realtime_lane_ref_factory(
        std::function<RealtimeLaneRef(LaneId)> factory);
    void handle_iv_module_instance_builders_changed(
        IvModuleInstanceBuildersChanged const &diff);
    std::vector<IvModuleSourceIntrospectionLiveInputSnapshot> collect_live_input_snapshots(
        std::vector<IvModuleSourceIntrospectionLiveInputSnapshotRequest> const &requests);
    void set_sample_input_value(
        ProjectSetSampleInputValueRequest const &request);
    void clear_sample_input_value_override(
        ProjectClearSampleInputValueOverrideRequest const &request);
    BuilderCompletionDiff complete_builder(
        std::string const &instance_id,
        GraphBuilder &builder);
};
} // namespace iv
