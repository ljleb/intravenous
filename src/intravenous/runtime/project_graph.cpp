#include <intravenous/runtime/project_graph.h>

#include <intravenous/graph/builder.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/project_graph_events.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_requests.h>
#include <intravenous/runtime/socket_rpc_response_builders.h>

#include <stdexcept>
#include <utility>

namespace iv {
ProjectGraph::ProjectGraph()
    : definitions_snapshot_(std::make_shared<NodeDefinitionsSnapshot const>())
{}

ProjectGraph::RebuildResult ProjectGraph::rebuild_locked(
    NodeInstancesMutation instance_mutation,
    GraphConnectionsMutation connection_mutation)
{
    GraphBuilder root_builder;
    NodeInstancesProjectGraphRequest request{
        .snapshot = definitions_snapshot_,
        .root_builder = &root_builder,
        .mutation = std::move(instance_mutation),
    };
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_graph_node_instances_requested_event,
        request);
    if (!request.handled) {
        throw std::runtime_error("ProjectGraph cannot rebuild without NodeInstances");
    }

    GraphConnectionsProjectGraphRequest connections_request{
        .root_builder = &root_builder,
        .placements = &request.result.placements,
        .mutation = std::move(connection_mutation),
    };
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_project_graph_connections_requested_event,
        connections_request);
    if (!connections_request.handled) {
        throw std::runtime_error("ProjectGraph cannot rebuild without GraphConnections");
    }

    root_builder.outputs();
    auto graph = std::make_shared<ConfiguredGraph const>(std::move(root_builder).finish());
    auto generation = std::make_shared<ProjectGraphGeneration>();
    generation->generation = next_generation_++;
    generation->definitions_generation = definitions_snapshot_->generation;
    generation->definitions = definitions_snapshot_;
    generation->graph = std::move(graph);
    generation->placements = std::move(request.result.placements);
    generation->diagnostics = std::move(request.result.diagnostics);
    generation->applied_connection_ids =
        std::move(connections_request.result.applied_connection_ids);
    generation->connection_diagnostics =
        std::move(connections_request.result.diagnostics);
    current_generation_ = generation;
    return RebuildResult{
        .generation = std::move(generation),
        .created_instance_ids = std::move(request.created_instance_ids),
    };
}

std::shared_ptr<ProjectGraphGeneration const> ProjectGraph::current_generation() const
{
    std::scoped_lock lock(mutex_);
    return current_generation_;
}

std::shared_ptr<NodeDefinitionsSnapshot const> ProjectGraph::definitions_snapshot() const
{
    std::scoped_lock lock(mutex_);
    return definitions_snapshot_;
}

void ProjectGraph::handle_node_definitions_snapshot_changed(
    NodeDefinitionsSnapshotChanged const& change)
{
    if (!change.snapshot) {
        throw std::invalid_argument("ProjectGraph received an empty definitions snapshot");
    }

    std::scoped_lock lock(mutex_);
    if (definitions_snapshot_
        && change.snapshot->generation <= definitions_snapshot_->generation) {
        return;
    }
    auto previous_snapshot = definitions_snapshot_;
    definitions_snapshot_ = change.snapshot;
    try {
        (void)rebuild_locked(std::monostate{}, std::monostate{});
    } catch (...) {
        // A snapshot is committed only together with the root generation built
        // from it. This also keeps an identical publication retryable after a
        // fatal wiring/structural failure.
        definitions_snapshot_ = std::move(previous_snapshot);
        throw;
    }
}

void ProjectGraph::handle_project_create_iv_module_instance(
    ProjectCreateIvModuleInstanceRequest const& request,
    ProjectStringBuilder& builder)
{
    std::string created_instance_id;
    {
        std::scoped_lock lock(mutex_);
        auto rebuilt = rebuild_locked(NodeInstanceCreateMutation{
            .instance_id = request.instance_id,
            .definition_id = request.module_id,
            .package_root = request.package_root,
            .display_name = request.display_name,
        }, std::monostate{});
        if (rebuilt.created_instance_ids.size() != 1) {
            throw std::logic_error("NodeInstances did not return one created instance id");
        }
        created_instance_id = std::move(rebuilt.created_instance_ids.front());
    }
    builder.succeed(std::move(created_instance_id));
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_delete_iv_module_instance(
    ProjectDeleteIvModuleInstanceRequest const& request,
    ProjectAckBuilder& builder)
{
    {
        std::scoped_lock lock(mutex_);
        (void)rebuild_locked(NodeInstanceDeleteMutation{
            .instance_id = request.instance_id,
        }, std::monostate{});
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_update_iv_module_instances(
    ProjectUpdateIvModuleInstancesRequest const& request,
    ProjectAckBuilder& builder)
{
    NodeInstanceUpdateMutation mutation;
    mutation.updates.reserve(request.updates.size());
    for (auto const& update : request.updates) {
        mutation.updates.push_back(NodeInstanceDisplayNameUpdate{
            .instance_id = update.instance_id,
            .display_name = update.display_name,
        });
    }
    {
        std::scoped_lock lock(mutex_);
        (void)rebuild_locked(std::move(mutation), std::monostate{});
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_upsert_graph_connection(
    ProjectUpsertGraphConnectionRequest const& request,
    ProjectAckBuilder& builder)
{
    {
        std::scoped_lock lock(mutex_);
        (void)rebuild_locked(
            std::monostate{},
            GraphConnectionUpsertMutation{.connection = request.connection});
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_delete_graph_connection(
    ProjectDeleteGraphConnectionRequest const& request,
    ProjectAckBuilder& builder)
{
    {
        std::scoped_lock lock(mutex_);
        (void)rebuild_locked(
            std::monostate{},
            GraphConnectionDeleteMutation{.connection_id = request.connection_id});
    }
    builder.succeed();
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_socket_rpc_create_iv_module_instance(
    CreateIvModuleInstanceRequest const& request,
    SocketRpcCreateIvModuleInstanceResultBuilder& builder)
{
    try {
        ProjectStringBuilder project_builder;
        handle_project_create_iv_module_instance(
            ProjectCreateIvModuleInstanceRequest{
                .module_id = request.module_id,
                .display_name = request.display_name,
            },
            project_builder);
        builder.succeed(project_builder.build());
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_delete_iv_module_instance(
    DeleteIvModuleInstanceRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_delete_iv_module_instance(
            ProjectDeleteIvModuleInstanceRequest{.instance_id = request.instance_id},
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_update_iv_module_instances(
    UpdateIvModuleInstancesRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        std::vector<ProjectUpdateIvModuleInstance> updates;
        updates.reserve(request.updates.size());
        for (auto const& update : request.updates) {
            updates.push_back(ProjectUpdateIvModuleInstance{
                .instance_id = update.instance_id,
                .display_name = update.display_name,
            });
        }
        ProjectAckBuilder project_builder;
        handle_project_update_iv_module_instances(
            ProjectUpdateIvModuleInstancesRequest{.updates = std::move(updates)},
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}
} // namespace iv
