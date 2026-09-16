#pragma once

#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/runtime/node_definition_types.h>
#include <intravenous/runtime/node_instances.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace iv {
class ProjectAckBuilder;
class ProjectStringBuilder;
class SocketRpcAckResponseBuilder;
class SocketRpcCreateIvModuleInstanceResultBuilder;
struct CreateIvModuleInstanceRequest;
struct DeleteIvModuleInstanceRequest;
struct NodeDefinitionsSnapshotChanged;
struct ProjectCreateIvModuleInstanceRequest;
struct ProjectDeleteIvModuleInstanceRequest;
struct ProjectUpdateIvModuleInstancesRequest;
struct UpdateIvModuleInstancesRequest;

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
};

// Root-graph transaction coordinator. It deliberately does not own desired
// instance state; NodeInstances remains the canonical owner and is invoked once
// per transaction through the ProjectGraph -> NodeInstances child edge.
class ProjectGraph {
    struct RebuildResult {
        std::shared_ptr<ProjectGraphGeneration const> generation{};
        std::vector<std::string> created_instance_ids{};
    };

    mutable std::mutex mutex_;
    std::shared_ptr<NodeDefinitionsSnapshot const> definitions_snapshot_{};
    std::shared_ptr<ProjectGraphGeneration const> current_generation_{};
    std::uint64_t next_generation_ = 1;

    RebuildResult rebuild_locked(NodeInstancesMutation mutation);

public:
    ProjectGraph();
    ~ProjectGraph() = default;
    ProjectGraph(ProjectGraph const&) = delete;
    ProjectGraph& operator=(ProjectGraph const&) = delete;

    [[nodiscard]] std::shared_ptr<ProjectGraphGeneration const> current_generation() const;
    [[nodiscard]] std::shared_ptr<NodeDefinitionsSnapshot const> definitions_snapshot() const;

    void handle_node_definitions_snapshot_changed(NodeDefinitionsSnapshotChanged const& change);
    void handle_project_create_iv_module_instance(
        ProjectCreateIvModuleInstanceRequest const& request,
        ProjectStringBuilder& builder);
    void handle_project_delete_iv_module_instance(
        ProjectDeleteIvModuleInstanceRequest const& request,
        ProjectAckBuilder& builder);
    void handle_project_update_iv_module_instances(
        ProjectUpdateIvModuleInstancesRequest const& request,
        ProjectAckBuilder& builder);

    void handle_socket_rpc_create_iv_module_instance(
        CreateIvModuleInstanceRequest const& request,
        SocketRpcCreateIvModuleInstanceResultBuilder& builder);
    void handle_socket_rpc_delete_iv_module_instance(
        DeleteIvModuleInstanceRequest const& request,
        SocketRpcAckResponseBuilder& builder);
    void handle_socket_rpc_update_iv_module_instances(
        UpdateIvModuleInstancesRequest const& request,
        SocketRpcAckResponseBuilder& builder);
};
} // namespace iv
