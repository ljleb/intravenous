#pragma once

#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/task_runner_events.h>
#include <intravenous/runtime/lane_graph.h>

#include <functional>
#include <atomic>
#include <memory>
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
    enum class LogicalSampleKnobState {
        overridden,
        timeline_lane,
    };

    enum class ConcreteSampleInputState {
        overridden,
        logical_follow,
        timeline_lane,
        disconnected,
    };

    enum class ConcreteEventInputState {
        default_,
        logical_follow,
        timeline_lane,
        disconnected,
    };

    // Output-state model (mirror of inputs, inverted). Disconnected is the default and is
    // represented by the *absence* of an entry in the state maps (never stored). A
    // disconnected output is left genuinely unconnected: no sink, no lane, nothing.
    enum class LogicalOutputState {
        timeline_lane,
    };

    enum class ConcreteOutputState {
        logical,
        timeline_lane,
    };

    struct DesiredGraphInputPort {
        std::string instance_id {};
        int module_instance_id = 0;
        GraphInputPortDescriptor port {};
        bool default_connected = false;
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
        std::vector<LaneId> prerequisite_lanes {};
    };

private:
    mutable std::mutex mutex;
    LaneIdAllocator lane_ids;
    std::unordered_map<std::string, std::vector<DesiredGraphInputPort>> desired_ports_by_instance_id;
    std::vector<DesiredGraphInputPort> desired_ports;
    std::unordered_map<std::string, std::vector<DesiredGraphInputPort>> desired_output_ports_by_instance_id;
    std::vector<DesiredGraphInputPort> desired_output_ports;
    std::unordered_map<std::string, LogicalOutputState> logical_output_states_by_key;
    std::unordered_map<std::string, ConcreteOutputState> concrete_output_states_by_key;
    std::vector<ExistingTrackedLane> tracked_lanes;
    std::unordered_map<std::string, std::vector<std::vector<Sample*>>> live_inputs;
    std::unordered_map<std::string, std::vector<std::unique_ptr<std::atomic<Sample::storage>>>> live_input_values;
    std::unordered_map<std::string, Sample> sample_input_default_values;
    std::unordered_set<std::string> concrete_live_input_overrides;
    std::unordered_map<std::string, LogicalSampleKnobState> logical_sample_knob_states_by_key;
    std::unordered_map<std::string, ConcreteSampleInputState> concrete_sample_input_states_by_key;
    std::unordered_map<std::string, ConcreteEventInputState> concrete_event_input_states_by_key;
    std::unordered_set<std::string> pending_rebuild_instance_ids;
    std::vector<TimelineLaneBatchUpdate> pending_timeline_batches;

    static std::vector<DesiredGraphInputPort> graph_input_port_descriptors_for(
        IvModuleInstance const &instance);
    static std::vector<DesiredGraphInputPort> graph_output_port_descriptors_for(
        IvModuleInstance const &instance);
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
    static std::string instance_port_state_key(
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
    static LaneMetadata graph_output_metadata(
        DesiredGraphInputPort const &port,
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
    std::atomic<Sample::storage>& ensure_live_input_value_locked(std::string_view key, size_t input_ordinal);
    std::atomic<Sample::storage>& ensure_live_input_value_initialized_locked(
        std::string_view key,
        size_t input_ordinal,
        Sample initial_value);
    Sample live_input_value_or_locked(std::string_view logical_node_id, size_t input_ordinal, Sample fallback) const;
    Sample live_input_value_or_locked(
        std::string_view logical_node_id,
        size_t member_ordinal,
        size_t input_ordinal,
        Sample fallback) const;
    std::atomic<Sample::storage> const* live_input_value_ptr_or_locked(std::string_view key, size_t input_ordinal);
    void mark_instances_requiring_rebuild_locked(
        std::string_view logical_node_id,
        std::optional<size_t> member_ordinal,
        size_t input_ordinal);
    void mark_instances_requiring_rebuild_for_logical_sample_input_locked(
        std::string_view logical_node_id,
        size_t input_ordinal);
    std::optional<DesiredGraphInputPort> find_desired_port_locked(
        std::string const &instance_id,
        GraphInputPortDescriptor const &port) const;
    GraphInputLaneBindings reconcile_ports_locked(TimelineLaneBatchUpdate *batch = nullptr);
    void reconcile_output_ports_locked(TimelineLaneBatchUpdate *batch = nullptr);
    std::optional<ConcreteOutputState> effective_concrete_output_state_locked(
        DesiredGraphInputPort const &port) const;
    LaneId graph_output_lane_for(
        GraphInputPortDescriptor const &port,
        bool logical_aggregation);
    void mark_instances_requiring_output_rebuild_locked(
        std::string_view logical_node_id,
        std::optional<size_t> member_ordinal,
        size_t output_ordinal,
        PortKind port_kind);
    void refresh_desired_ports_locked();
    void refresh_desired_output_ports_locked();
    GraphInputLaneBindings sample_input_bindings(
        std::string const &node_id,
        std::optional<size_t> member_ordinal,
        size_t input_ordinal);
    GraphInputLaneBindings query_graph_input_lane_bindings(
        ProjectGraphInputLaneBindingsRequest const &request);
    std::optional<LaneMetadata> tracked_lane_metadata_locked(LaneId lane) const;
    void apply_tracked_batch_locked(TimelineLaneBatchUpdate const &batch);
    void queue_timeline_batch_locked(TimelineLaneBatchUpdate const &batch);
    std::vector<TimelineLaneBatchUpdate> take_pending_timeline_batches_locked();
    void apply_timeline_batch(TimelineLaneBatchUpdate const &batch);

public:
    GraphInputLanes() = default;

    void handle_iv_module_instance_builders_changed(
        IvModuleInstanceBuildersChanged const &diff,
        IvModuleInstanceBuildersAckBuilder *ack_builder = nullptr);
    std::vector<IvModuleSourceIntrospectionLiveInputSnapshot> collect_live_input_snapshots(
        std::vector<IvModuleSourceIntrospectionLiveInputSnapshotRequest> const &requests);
    void set_sample_input_value(
        ProjectSetSampleInputValueRequest const &request);
    void set_sample_input_state(
        ProjectSetSampleInputStateRequest const &request);
    void set_event_input_state(
        ProjectSetEventInputStateRequest const &request);
    void set_sample_output_state(
        ProjectSetSampleOutputStateRequest const &request);
    void set_event_output_state(
        ProjectSetEventOutputStateRequest const &request);
    [[nodiscard]] GraphInputLaneBindings graph_input_lane_bindings(
        ProjectGraphInputLaneBindingsRequest const &request);
    void handle_task_runner_pass_finished(TaskRunnerPassFinished const &finished);
    BuilderCompletionDiff complete_builder(
        std::string const &instance_id,
        GraphBuilder &builder);
};
} // namespace iv
