#pragma once

#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/node_definition_types.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/node_event_protocol_types.h>
#include <intravenous/node/message.h>

#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace iv {
class ProjectAckBuilder;
class ProjectPersistenceBuilder;
class ProjectStringBuilder;
class SocketRpcAckResponseBuilder;
class SocketRpcCreateNodeInstanceResultBuilder;
class SocketRpcNodeMessageResultBuilder;
class SocketRpcNodeEventProtocolResultBuilder;
struct CreateNodeInstanceRequest;
struct DeleteNodeInstanceRequest;
struct NodeDefinitionsSnapshotChanged;
struct ProjectCreateNodeInstanceRequest;
struct ProjectDeleteNodeInstanceRequest;
struct ProjectUpdateNodeInstancesRequest;
struct ProjectUpsertGraphConnectionRequest;
struct ProjectDeleteGraphConnectionRequest;
struct ProjectReplaceGraphRequest;
struct ProjectUpsertManualControlRequest;
struct ProjectDeleteManualControlRequest;
struct ProjectSetManualControlHoverRequest;
struct ProjectBeginManualControlInteractionRequest;
struct ProjectUpdateManualControlInteractionRequest;
struct ProjectEndManualControlInteractionRequest;
struct UpdateNodeInstancesRequest;
struct UpsertGraphConnectionRequest;
struct DeleteGraphConnectionRequest;
struct UpsertManualControlRequest;
struct DeleteManualControlRequest;
struct SetManualControlHoverRequest;
struct BeginManualControlInteractionRequest;
struct UpdateManualControlInteractionRequest;
struct EndManualControlInteractionRequest;
struct DispatchNodeMessageRequest;
struct GetNodeEventProtocolRequest;

// Wall-clock microseconds of the rebuild that produced a generation, from the
// rebuild's start to GraphExecutor accepting (or refusing) the compiled graph.
struct ProjectGraphRebuildTimings {
    std::uint64_t node_instances_us = 0;
    std::uint64_t graph_connections_us = 0;
    // Root finish plus generation assembly (placements, controls, handlers).
    std::uint64_t finish_us = 0;
    std::uint64_t graph_jit_us = 0;
    std::uint64_t executor_stage_us = 0;
    std::uint64_t total_us = 0;
};

// One immutable root-configuration generation produced by ProjectGraph. GraphJit
// will consume this object (plus its pinned definition provenance) in the next
// execution-side checkpoint.
struct ProjectGraphGeneration {
    std::uint64_t generation = 0;
    std::uint64_t definitions_generation = 0;
    std::shared_ptr<NodeDefinitionsSnapshot const> definitions{};
    std::shared_ptr<ConfiguredGraph const> graph{};
    std::unordered_map<std::string, NodeInstancePlacement> placements{};
    std::vector<std::string> applied_connection_ids{};
    std::vector<GraphConnectionDiagnostic> connection_diagnostics{};
    bool graph_jit_attempted = false;
    std::shared_ptr<CompiledGraph const> compiled_graph{};
    std::vector<GraphJitDiagnostic> graph_jit_diagnostics{};
    ProjectGraphRebuildTimings rebuild_timings{};
    // One entry per GraphJit compilation of this rebuild (see GraphJitCompileTimings).
    std::vector<GraphJitCompileTimings> graph_jit_timings{};
    std::vector<BoundNodeMessageHandler> message_handlers{};
    std::vector<ProjectNodeReplacement> node_replacements{};
    std::vector<ProjectManualControl> manual_controls{};
    struct ManualControlDiagnostic {
        ManualControlId id{};
        std::string message{};
    };
    std::vector<ManualControlDiagnostic> manual_control_diagnostics{};
    bool graph_executor_attempted = false;
    bool graph_executor_accepted = false;
};

// The latest configuration pass that failed. The desired revision it tried to
// realize stays owned by NodeInstances and GraphConnections; the previous
// generation stays current and keeps running. Cleared by the next successful
// pass.
struct ProjectGraphConfigurationFailure {
    std::uint64_t definitions_generation = 0;
    // Every problem found before construction, or the first construction
    // failure.
    std::vector<NodeInstanceDiagnostic> node_instance_diagnostics{};
    // A resolved, compatible project connection that failed to apply.
    std::optional<GraphConnectionDiagnostic> connection_failure{};
};

struct NodeMessageDiagnostic {
    NodeAddress target{};
    std::string message{};
};

struct NodeMessageDispatchResult {
    std::vector<NodeMessageDiagnostic> diagnostics{};
    bool project_state_changed = false;
};

struct SerializedNodeMessage {
    std::string event_type_nominal_id{};
    std::string event_type_definition_fingerprint{};
    std::shared_ptr<nlohmann::ordered_json const> payload{};
};

// Root-graph transaction coordinator. It deliberately does not own desired
// instance or connection state. NodeInstances and GraphConnections remain the
// canonical sibling owners and are each invoked once per transaction through
// ProjectGraph child edges.
class ProjectGraph {
    struct NodeMessageInteraction;
    struct RebuildResult {
        // Null when the configuration pass failed.
        std::shared_ptr<ProjectGraphGeneration const> generation{};
        std::vector<std::string> created_instance_ids{};
        std::shared_ptr<ProjectGraphConfigurationFailure const> failure{};
    };
    struct ActiveManualControlInteraction {
        ManualControlInteractionId interaction{};
        ManualControlId control{};
    };
    struct ControlCompileRequest {
        std::uint64_t request_id = 0;
        std::uint64_t project_generation = 0;
        std::shared_ptr<ConfiguredGraph const> graph{};
        std::shared_ptr<NodeDefinitionsSnapshot const> definitions{};
        std::vector<ManualControlId> dynamic_inputs{};
        bool activate_on_ready = false;
    };
    struct ReadyControlCode {
        std::uint64_t project_generation = 0;
        std::vector<ManualControlId> dynamic_inputs{};
        std::shared_ptr<CompiledGraph const> graph{};
    };
    struct InflightControlCompile {
        std::uint64_t request_id = 0;
        std::uint64_t project_generation = 0;
        std::vector<ManualControlId> dynamic_inputs{};
        bool activate_on_ready = false;
    };

    mutable std::mutex mutex_;
    std::shared_ptr<NodeDefinitionsSnapshot const> definitions_snapshot_{};
    std::shared_ptr<ProjectGraphGeneration const> current_generation_{};
    std::shared_ptr<ProjectGraphConfigurationFailure const> configuration_failure_{};
    std::shared_ptr<CompiledGraph const> accepted_execution_graph_{};
    std::vector<ProjectManualControl> manual_controls_{};
    std::vector<ManualControlId> dynamic_inputs_{};
    std::vector<ActiveManualControlInteraction> manual_interactions_{};
    std::unordered_map<std::string, ManualControlId> hovered_controls_{};
    std::optional<ControlCompileRequest> pending_control_compile_{};
    std::optional<InflightControlCompile> inflight_control_compile_{};
    std::optional<ReadyControlCode> ready_control_code_{};
    std::optional<std::chrono::steady_clock::time_point>
        constant_specialization_deadline_{};
    std::condition_variable control_compile_changed_{};
    std::uint64_t next_control_compile_request_ = 1;
    std::uint64_t latest_control_compile_request_ = 0;
    std::uint64_t next_generation_ = 1;
    std::jthread control_compile_worker_{};

    static constexpr auto manual_control_idle_debounce =
        std::chrono::milliseconds(250);

    RebuildResult rebuild_locked(
        NodeInstancesMutation instance_mutation,
        GraphConnectionsMutation connection_mutation,
        bool publish_execution = true);
    void dispatch_node_message_locked(
        NodeAddress const& target,
        OwnedNodeMessage message,
        NodeMessageInteraction& interaction,
        NodeMessageDispatchResult& result);
    void dispatch_root_node_message_locked(
        NodeAddress const& target,
        OwnedNodeMessage message,
        NodeMessageDispatchResult& result);
    [[nodiscard]] std::vector<ManualControlId>
    desired_dynamic_inputs_locked(bool include_predictive_hover) const;
    void schedule_control_compile_locked(
        std::vector<ManualControlId> dynamic_inputs,
        bool activate_on_ready);
    void control_compile_worker(std::stop_token stop);
    [[nodiscard]] ProjectManualControl* find_manual_control_locked(
        ManualControlId const& id);
    [[nodiscard]] bool stage_ready_control_code_locked(
        std::span<ManualControlId const> dynamic_inputs);
    void set_manual_control_target_locked(
        ManualControlId const& id, Sample value);
    void reconcile_manual_control_activity_locked(
        bool settled_dynamic_input);

public:
    ProjectGraph();
    ~ProjectGraph();
    ProjectGraph(ProjectGraph const&) = delete;
    ProjectGraph& operator=(ProjectGraph const&) = delete;

    [[nodiscard]] std::shared_ptr<ProjectGraphGeneration const> current_generation() const;
    // The failure of the latest configuration pass, or null when it succeeded.
    [[nodiscard]] std::shared_ptr<ProjectGraphConfigurationFailure const>
    configuration_failure() const;
    // The desired manual controls, which outlive a failed configuration pass.
    [[nodiscard]] std::vector<ProjectManualControl> manual_controls() const;
    [[nodiscard]] std::shared_ptr<NodeDefinitionsSnapshot const> definitions_snapshot() const;
    [[nodiscard]] NodeMessageDispatchResult dispatch_node_message(
        NodeAddress const& target, OwnedNodeMessage message);
    [[nodiscard]] NodeMessageDispatchResult dispatch_node_message(
        NodeAddress const& target, SerializedNodeMessage const& message);
    [[nodiscard]] NodeEventProtocolSnapshot node_event_protocol() const;

    void handle_node_definitions_snapshot_changed(NodeDefinitionsSnapshotChanged const& change);
    void handle_project_create_node_instance(
        ProjectCreateNodeInstanceRequest const& request,
        ProjectStringBuilder& builder);
    void handle_project_delete_node_instance(
        ProjectDeleteNodeInstanceRequest const& request,
        ProjectAckBuilder& builder);
    void handle_project_update_node_instances(
        ProjectUpdateNodeInstancesRequest const& request,
        ProjectAckBuilder& builder);

    void handle_project_upsert_graph_connection(
        ProjectUpsertGraphConnectionRequest const& request,
        ProjectAckBuilder& builder);
    void handle_project_delete_graph_connection(
        ProjectDeleteGraphConnectionRequest const& request,
        ProjectAckBuilder& builder);
    void handle_project_replace_graph(
        ProjectReplaceGraphRequest const& request,
        ProjectAckBuilder& builder);
    void handle_project_upsert_manual_control(
        ProjectUpsertManualControlRequest const& request,
        ProjectAckBuilder& builder);
    void handle_project_delete_manual_control(
        ProjectDeleteManualControlRequest const& request,
        ProjectAckBuilder& builder);
    void handle_project_set_manual_control_hover(
        ProjectSetManualControlHoverRequest const& request,
        ProjectAckBuilder& builder);
    void handle_project_begin_manual_control_interaction(
        ProjectBeginManualControlInteractionRequest const& request,
        ProjectAckBuilder& builder);
    void handle_project_update_manual_control_interaction(
        ProjectUpdateManualControlInteractionRequest const& request,
        ProjectAckBuilder& builder);
    void handle_project_end_manual_control_interaction(
        ProjectEndManualControlInteractionRequest const& request,
        ProjectAckBuilder& builder);
    void handle_project_persistence_collect_state(
        ProjectPersistenceBuilder& builder) const;

    void handle_socket_rpc_create_node_instance(
        CreateNodeInstanceRequest const& request,
        SocketRpcCreateNodeInstanceResultBuilder& builder);
    void handle_socket_rpc_delete_node_instance(
        DeleteNodeInstanceRequest const& request,
        SocketRpcAckResponseBuilder& builder);
    void handle_socket_rpc_update_node_instances(
        UpdateNodeInstancesRequest const& request,
        SocketRpcAckResponseBuilder& builder);
    void handle_socket_rpc_upsert_graph_connection(
        UpsertGraphConnectionRequest const& request,
        SocketRpcAckResponseBuilder& builder);
    void handle_socket_rpc_delete_graph_connection(
        DeleteGraphConnectionRequest const& request,
        SocketRpcAckResponseBuilder& builder);
    void handle_socket_rpc_upsert_manual_control(
        UpsertManualControlRequest const& request,
        SocketRpcAckResponseBuilder& builder);
    void handle_socket_rpc_delete_manual_control(
        DeleteManualControlRequest const& request,
        SocketRpcAckResponseBuilder& builder);
    void handle_socket_rpc_set_manual_control_hover(
        SetManualControlHoverRequest const& request,
        SocketRpcAckResponseBuilder& builder);
    void handle_socket_rpc_begin_manual_control_interaction(
        BeginManualControlInteractionRequest const& request,
        SocketRpcAckResponseBuilder& builder);
    void handle_socket_rpc_update_manual_control_interaction(
        UpdateManualControlInteractionRequest const& request,
        SocketRpcAckResponseBuilder& builder);
    void handle_socket_rpc_end_manual_control_interaction(
        EndManualControlInteractionRequest const& request,
        SocketRpcAckResponseBuilder& builder);
    void handle_socket_rpc_client_disconnected(std::string const& client_id);
    void handle_socket_rpc_dispatch_node_message(
        DispatchNodeMessageRequest const& request,
        SocketRpcNodeMessageResultBuilder& builder);
    void handle_socket_rpc_get_node_event_protocol(
        GetNodeEventProtocolRequest const& request,
        SocketRpcNodeEventProtocolResultBuilder& builder);
};
} // namespace iv
