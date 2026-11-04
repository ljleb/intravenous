#pragma once

#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/node_definition_types.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/node_event_protocol_types.h>
#include <intravenous/node/message.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace iv {
class ProjectAckBuilder;
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
struct UpdateNodeInstancesRequest;
struct UpsertGraphConnectionRequest;
struct DeleteGraphConnectionRequest;
struct DispatchNodeMessageRequest;
struct GetNodeEventProtocolRequest;

// One immutable root-configuration generation produced by ProjectGraph. GraphJit
// will consume this object (plus its pinned definition provenance) in the next
// execution-side checkpoint.
struct ProjectGraphGeneration {
    std::uint64_t generation = 0;
    std::uint64_t definitions_generation = 0;
    std::shared_ptr<NodeDefinitionsSnapshot const> definitions{};
    std::shared_ptr<ConfiguredGraph const> graph{};
    std::unordered_map<std::string, NodeInstancePlacement> placements{};
    std::vector<NodeInstanceDiagnostic> diagnostics{};
    std::vector<std::string> applied_connection_ids{};
    std::vector<GraphConnectionDiagnostic> connection_diagnostics{};
    bool graph_jit_attempted = false;
    std::shared_ptr<CompiledGraph const> compiled_graph{};
    std::vector<GraphJitDiagnostic> graph_jit_diagnostics{};
    std::vector<BoundNodeMessageHandler> message_handlers{};
    std::vector<ProjectNodeReplacement> node_replacements{};
    bool graph_executor_attempted = false;
    bool graph_executor_accepted = false;
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
        std::shared_ptr<ProjectGraphGeneration const> generation{};
        std::vector<std::string> created_instance_ids{};
    };

    mutable std::mutex mutex_;
    std::shared_ptr<NodeDefinitionsSnapshot const> definitions_snapshot_{};
    std::shared_ptr<ProjectGraphGeneration const> current_generation_{};
    std::shared_ptr<CompiledGraph const> accepted_execution_graph_{};
    std::uint64_t next_generation_ = 1;

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

public:
    ProjectGraph();
    ~ProjectGraph() = default;
    ProjectGraph(ProjectGraph const&) = delete;
    ProjectGraph& operator=(ProjectGraph const&) = delete;

    [[nodiscard]] std::shared_ptr<ProjectGraphGeneration const> current_generation() const;
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
    void handle_socket_rpc_dispatch_node_message(
        DispatchNodeMessageRequest const& request,
        SocketRpcNodeMessageResultBuilder& builder);
    void handle_socket_rpc_get_node_event_protocol(
        GetNodeEventProtocolRequest const& request,
        SocketRpcNodeEventProtocolResultBuilder& builder);
};
} // namespace iv
