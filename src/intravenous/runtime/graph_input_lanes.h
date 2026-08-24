#pragma once

#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_instances_events.h>
#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/sample_stream_blocks.h>
#include <intravenous/runtime/task_runner_events.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/runtime/graph_input_lanes/block_store.h>
#include <intravenous/runtime/uuid.h>

#include <functional>
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
    enum class VirtualSampleKnobState {
        overridden,
        timeline_lane,
    };

    enum class NodeBundleSampleInputState {
        overridden,
        virtual_follow,
        timeline_lane,
        disconnected,
    };

    enum class NodeBundleEventInputState {
        default_,
        virtual_follow,
        timeline_lane,
        disconnected,
    };

    enum class VirtualOutputState {
        timeline_lane,
    };

    enum class NodeBundleOutputState {
        virtual_port,
        timeline_lane,
    };

    struct DesiredGraphPort {
        std::string instance_id {};
        int module_instance_id = 0;
        GraphInputPortDescriptor port {};
        bool authored_connected = false;
        Sample default_value = 0.0f;
        std::optional<Sample> min {};
        std::optional<Sample> max {};
    };

    struct DesiredPublicGraphPortChannel {
        std::optional<size_t> port_ordinal {};
    };

    struct DesiredPublicGraphPort {
        std::string instance_id {};
        int module_instance_id = 0;
        bool input = true;
        PortKind port_kind = PortKind::sample;
        size_t port_ordinal = 0;
        std::string port_name {};
        std::string port_type {};
        std::optional<ChannelTypeId> sample_channel_type {};
        std::optional<EventTypeId> event_type {};
        Sample default_value = 0.0f;
        std::optional<Sample> min {};
        std::optional<Sample> max {};
        std::vector<SourceInfo> source_infos {};
        std::string source_identity {};
        std::optional<int> source_identity_hash {};
        std::optional<int> public_port_name_hash {};
        std::optional<size_t> node_bundle_port_ordinal {};
        bool graph_connected = false;
        std::vector<DesiredPublicGraphPortChannel> channels {};
    };

    struct ExistingTrackedLane {
        LaneId lane {};
        InternedString external_id {};
        LaneMetadata metadata {};
    };

    struct AuthoredStateSnapshot {
        std::vector<ProjectSetSampleInputValueRequest> sample_input_values {};
        std::vector<ProjectSetSampleInputStateRequest> sample_input_states {};
        std::vector<ProjectSetEventInputStateRequest> event_input_states {};
        std::vector<ProjectSetSampleOutputStateRequest> sample_output_states {};
        std::vector<ProjectSetEventOutputStateRequest> event_output_states {};
    };

private:
    mutable std::mutex mutex;
    GraphInputLanesBlockStore output_blocks_;
    LaneIdAllocator lane_ids;
    std::unordered_map<std::string, std::vector<DesiredGraphPort>> desired_ports_by_instance_id;
    std::vector<DesiredGraphPort> desired_ports;
    std::unordered_map<std::string, std::vector<DesiredGraphPort>> desired_output_ports_by_instance_id;
    std::vector<DesiredGraphPort> desired_output_ports;
    std::unordered_map<std::string, std::vector<DesiredPublicGraphPort>> desired_public_input_ports_by_instance_id;
    std::vector<DesiredPublicGraphPort> desired_public_input_ports;
    std::unordered_map<std::string, std::vector<DesiredPublicGraphPort>> desired_public_output_ports_by_instance_id;
    std::vector<DesiredPublicGraphPort> desired_public_output_ports;
    std::unordered_map<std::string, VirtualOutputState> virtual_output_states_by_key;
    std::unordered_map<std::string, NodeBundleOutputState> node_bundle_output_states_by_key;
    std::vector<ExistingTrackedLane> tracked_lanes;
    std::unordered_map<std::string, std::vector<std::optional<Sample>>> live_input_values;
    std::unordered_map<std::string, Sample> sample_input_default_values;
    std::unordered_set<std::string> node_bundle_live_input_overrides;
    std::unordered_map<std::string, VirtualSampleKnobState> virtual_sample_knob_states_by_key;
    std::unordered_map<std::string, NodeBundleSampleInputState> node_bundle_sample_input_states_by_key;
    std::unordered_map<std::string, NodeBundleEventInputState> node_bundle_event_input_states_by_key;
    std::unordered_map<std::string, InternedString> virtual_sample_knob_lane_ids_by_key;
    std::unordered_map<std::string, InternedString> node_bundle_sample_input_lane_ids_by_key;
    std::unordered_map<std::string, InternedString> virtual_event_input_lane_ids_by_key;
    std::unordered_map<std::string, InternedString> node_bundle_event_input_lane_ids_by_key;
    std::unordered_map<std::string, InternedString> virtual_output_lane_ids_by_key;
    std::unordered_map<std::string, InternedString> node_bundle_output_lane_ids_by_key;
    std::unordered_map<std::string, ProjectSampleInputState> public_sample_input_states_by_key;
    std::unordered_map<std::string, InternedString> public_sample_input_lane_ids_by_key;
    std::unordered_map<std::string, ProjectEventInputState> public_event_input_states_by_key;
    std::unordered_map<std::string, InternedString> public_event_input_lane_ids_by_key;
    std::unordered_map<std::string, ProjectSampleOutputState> public_sample_output_states_by_key;
    std::unordered_map<std::string, ProjectEventOutputState> public_event_output_states_by_key;
    std::unordered_map<std::string, Sample> public_sample_input_values;
    std::unordered_map<std::string, std::shared_ptr<GraphRuntimeBindings>>
        runtime_bindings_by_instance_id;
    std::unordered_set<std::string> pending_runtime_binding_syncs;
    std::vector<TimelineLaneBatchUpdate> pending_timeline_batches;
    std::uint64_t current_update_version_index_ = 1;

    static std::vector<DesiredGraphPort> graph_input_port_descriptors_for(
        IvModuleInstance const &instance);
    static std::vector<DesiredGraphPort> graph_output_port_descriptors_for(
        IvModuleInstance const &instance);
    static std::vector<DesiredPublicGraphPort> public_graph_input_ports_for(
        IvModuleInstance const &instance);
    static std::vector<DesiredPublicGraphPort> public_graph_output_ports_for(
        IvModuleInstance const &instance);
    static int module_instance_numeric_id(std::string_view instance_id);
    static int hash_string(std::string const &value);
    static std::string node_bundle_key(std::string_view virtual_node_id, size_t member_ordinal);
    static std::string node_bundle_key_prefix(std::string_view virtual_node_id);
    static std::string node_bundle_override_key(
        std::string_view virtual_node_id,
        size_t member_ordinal,
        size_t input_ordinal);
    static std::string desired_port_key(DesiredGraphPort const &port);
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
        size_t input_ordinal,
        ChannelTypeId channel_type = ChannelTypeId::mono);
    static LaneMetadata graph_input_metadata(
        DesiredGraphPort const &port,
        bool knob,
        bool is_virtual,
        bool concrete,
        bool sample,
        bool event);
    static LaneMetadata graph_output_metadata(
        DesiredGraphPort const &port,
        bool is_virtual,
        bool concrete,
        bool sample,
        bool event);
    static bool lane_metadata_matches_port(
        LaneMetadata const &metadata,
        DesiredGraphPort const &port);
    static bool has_node_bundle_descriptor_for_port(
        std::span<DesiredGraphPort const> ports,
        DesiredGraphPort const &virtual_port);
    static std::string public_port_key(DesiredPublicGraphPort const &port);
    static std::string public_sample_input_state_key(
        std::string_view instance_id,
        std::string_view source_identity,
        std::optional<size_t> member_ordinal);
    static std::string public_port_external_id(DesiredPublicGraphPort const &port);
    static LaneMetadata public_graph_port_metadata(
        DesiredPublicGraphPort const &port,
        bool sample,
        bool event);
    Sample& ensure_live_input_value_locked(std::string_view key, size_t input_ordinal);
    Sample& ensure_live_input_value_initialized_locked(
        std::string_view key,
        size_t input_ordinal,
        Sample initial_value
    );
    Sample live_input_value_or_locked(std::string_view virtual_node_id, size_t input_ordinal, Sample fallback) const;
    Sample live_input_value_or_locked(
        std::string_view virtual_node_id,
        size_t member_ordinal,
        size_t input_ordinal,
        Sample fallback) const;
    void schedule_instances_for_input_locked(
        std::string_view virtual_node_id,
        std::optional<size_t> member_ordinal,
        size_t input_ordinal);
    void schedule_instances_for_virtual_sample_input_locked(
        std::string_view virtual_node_id,
        size_t input_ordinal);
    std::optional<DesiredGraphPort> find_desired_port_locked(
        std::string const &instance_id,
        GraphInputPortDescriptor const &port) const;
    GraphInputLaneBindings reconcile_ports_locked(TimelineLaneBatchUpdate *batch = nullptr);
    void reconcile_output_ports_locked(TimelineLaneBatchUpdate *batch = nullptr);
    std::optional<NodeBundleOutputState> effective_node_bundle_output_state_locked(
        DesiredGraphPort const &port) const;
    bool virtual_output_is_timeline_lane_locked(
        DesiredGraphPort const &port) const;
    LaneId graph_output_lane_for(
        GraphInputPortDescriptor const &port,
        bool virtual_aggregation);
    void schedule_instances_for_output_locked(
        std::string_view virtual_node_id,
        std::optional<size_t> member_ordinal,
        size_t output_ordinal,
        PortKind port_kind);
    void sync_runtime_bindings_locked(std::string const &instance_id);
    void schedule_runtime_binding_sync_locked(std::string const &instance_id);
    std::vector<LaneId> prerequisite_lanes_for_instance_locked(
        std::string const &instance_id) const;
    void refresh_desired_ports_locked();
    void refresh_desired_output_ports_locked();
    void refresh_desired_public_input_ports_locked();
    void refresh_desired_public_output_ports_locked();
    void reconcile_public_ports_locked(TimelineLaneBatchUpdate *batch = nullptr);
    LaneId public_graph_port_lane_for(DesiredPublicGraphPort const &port) const;
    std::optional<LaneId> effective_public_sample_input_lane_locked(
        DesiredPublicGraphPort const &port) const;
    std::optional<LaneId> effective_public_event_input_lane_locked(
        DesiredPublicGraphPort const &port) const;
    Sample& ensure_public_sample_input_value_locked(
        std::string const& instance_id,
        std::string const& source_identity,
        Sample default_value
    );
    GraphInputLaneBindings sample_input_bindings(
        std::string const &node_id,
        std::optional<size_t> member_ordinal,
        size_t input_ordinal,
        ChannelTypeId channel_type);
    GraphInputLaneBindings query_graph_input_lane_bindings(
        ProjectGraphInputLaneBindingsRequest const &request);
    std::optional<LaneMetadata> tracked_lane_metadata_locked(LaneId lane) const;
    void apply_tracked_batch_locked(TimelineLaneBatchUpdate const &batch);
    void queue_timeline_batch_locked(TimelineLaneBatchUpdate const &batch);
    std::vector<TimelineLaneBatchUpdate> take_pending_timeline_batches_locked();
    void apply_timeline_batch(TimelineLaneBatchUpdate const &batch);
    void publish_sample_output_block(LaneId lane, BorrowedSampleBlock const &block);
    void publish_event_output_block(LaneId lane, std::span<TimedEvent const> events);
    BorrowedSampleBlock sample_output_block(LaneId lane) const;
    std::span<TimedEvent const> event_output_block(LaneId lane) const;

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
    void set_public_sample_input_state(
        ProjectSetPublicSampleInputStateRequest const &request);
    void set_public_sample_input_value(
        std::string const &instance_id,
        std::string const &source_identity,
        Sample value);
    void set_public_event_input_state(
        std::string const &instance_id,
        std::string const &source_identity,
        std::optional<size_t> member_ordinal,
        ProjectEventInputState state,
        std::optional<InternedString> lane_id = std::nullopt);
    std::vector<PublicSampleInputInfo> public_sample_inputs() const;
    std::vector<PublicEventInputInfo> public_event_inputs() const;
    std::vector<PublicSampleOutputInfo> public_sample_outputs() const;
    std::vector<PublicEventOutputInfo> public_event_outputs() const;
    void set_event_input_state(
        ProjectSetEventInputStateRequest const &request);
    void set_sample_output_state(
        ProjectSetSampleOutputStateRequest const &request);
    void set_event_output_state(
        ProjectSetEventOutputStateRequest const &request);
    [[nodiscard]] GraphInputLaneBindings graph_input_lane_bindings(
        ProjectGraphInputLaneBindingsRequest const &request);
    [[nodiscard]] AuthoredStateSnapshot authored_state() const;
    void handle_task_runner_after_pass(TasksRunnerAfterPass const &finished);
    void handle_sample_block_published(LaneId lane, BorrowedSampleBlock const &block);
    void handle_event_block_published(LaneId lane, std::span<TimedEvent const> events);
    void prepare_sample_output_block(LaneId lane);
    void prepare_event_output_block(LaneId lane);
    BorrowedSampleBlock handle_sample_block_requested(LaneId lane) const;
    std::span<TimedEvent const> handle_event_block_requested(LaneId lane) const;
};
} // namespace iv
