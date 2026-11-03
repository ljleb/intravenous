#include <intravenous/runtime/project_graph.h>

#include <intravenous/graph/builder.h>
#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/project_graph_events.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/socket_rpc_requests.h>
#include <intravenous/runtime/socket_rpc_response_builders.h>

#include <stdexcept>
#include <unordered_set>
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
    generation->generation = next_generation_;
    generation->definitions_generation = definitions_snapshot_->generation;
    generation->definitions = definitions_snapshot_;
    generation->graph = std::move(graph);
    generation->placements = std::move(request.result.placements);
    generation->diagnostics = std::move(request.result.diagnostics);
    generation->applied_connection_ids =
        std::move(connections_request.result.applied_connection_ids);
    generation->connection_diagnostics =
        std::move(connections_request.result.diagnostics);

    auto graph_jit_result = IV_INVOKE_SINGLETON_EVENT(
        iv_runtime_project_graph_graph_jit_requested_event,
        ProjectGraphGraphJitRequest{
            .successor = {
                .project_generation = generation->generation,
                .graph = generation->graph,
                .definitions = generation->definitions,
            },
            .predecessor = accepted_execution_graph_,
        });
    generation->graph_jit_attempted = graph_jit_result.attempted;
    generation->compiled_graph = std::move(graph_jit_result.compiled_graph);
    generation->graph_jit_diagnostics = std::move(graph_jit_result.diagnostics);
    if (generation->compiled_graph) {
        auto const execution_result = IV_INVOKE_SINGLETON_EVENT(
            iv_runtime_project_graph_graph_executor_requested_event,
            generation->compiled_graph);
        generation->graph_executor_attempted = execution_result.attempted;
        generation->graph_executor_accepted = execution_result.accepted;
        if (generation->graph_executor_accepted) {
            accepted_execution_graph_ = generation->compiled_graph;
        }
    }

    current_generation_ = generation;
    ++next_generation_;
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

void ProjectGraph::handle_project_create_node_instance(
    ProjectCreateNodeInstanceRequest const& request,
    ProjectStringBuilder& builder)
{
    std::string created_instance_id;
    {
        std::scoped_lock lock(mutex_);
        auto rebuilt = rebuild_locked(NodeInstanceCreateMutation{
            .instance_id = request.instance_id,
            .definition_id = request.definition_id,
            .package_root = request.package_root,
            .display_name = request.display_name,
            .configuration_source = request.configuration_source,
        }, std::monostate{});
        if (rebuilt.created_instance_ids.size() != 1) {
            throw std::logic_error("NodeInstances did not return one created instance id");
        }
        created_instance_id = std::move(rebuilt.created_instance_ids.front());
    }
    builder.succeed(std::move(created_instance_id));
    IV_INVOKE_LINKER_EVENT(iv_runtime_project_state_changed_event);
}

void ProjectGraph::handle_project_delete_node_instance(
    ProjectDeleteNodeInstanceRequest const& request,
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

void ProjectGraph::handle_project_update_node_instances(
    ProjectUpdateNodeInstancesRequest const& request,
    ProjectAckBuilder& builder)
{
    NodeInstanceUpdateMutation mutation;
    mutation.updates.reserve(request.updates.size());
    for (auto const& update : request.updates) {
        mutation.updates.push_back(NodeInstanceDisplayNameUpdate{
            .instance_id = update.instance_id,
            .display_name = update.display_name,
            .configuration_source = update.configuration_source,
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

void ProjectGraph::handle_project_replace_graph(
    ProjectReplaceGraphRequest const& request,
    ProjectAckBuilder& builder)
{
    std::unordered_set<std::string> instance_ids;
    for (auto const& instance : request.instances) {
        if (instance.instance_id.empty()) {
            throw std::invalid_argument("project node instance id must not be empty");
        }
        if (instance.definition_id.empty()) {
            throw std::invalid_argument("project node definition id must not be empty");
        }
        if (!instance_ids.insert(instance.instance_id).second) {
            throw std::invalid_argument(
                "duplicate project node instance id in replacement batch: "
                + instance.instance_id);
        }
    }
    std::unordered_set<std::string> connection_ids;
    for (auto const& connection : request.connections) {
        validate_project_connection(connection);
        auto const& connection_id = project_connection_id(connection);
        if (!connection_ids.insert(connection_id).second) {
            throw std::invalid_argument(
                "duplicate project connection id in replacement batch: "
                + connection_id);
        }
    }
    {
        std::scoped_lock lock(mutex_);
        (void)rebuild_locked(
            NodeInstanceReplaceAllMutation{.instances = request.instances},
            GraphConnectionReplaceAllMutation{.connections = request.connections});
    }
    builder.succeed();
}

void ProjectGraph::handle_socket_rpc_create_node_instance(
    CreateNodeInstanceRequest const& request,
    SocketRpcCreateNodeInstanceResultBuilder& builder)
{
    try {
        ProjectStringBuilder project_builder;
        handle_project_create_node_instance(
            ProjectCreateNodeInstanceRequest{
                .instance_id = request.instance_id,
                .definition_id = request.definition_id,
                .display_name = request.display_name,
                .configuration_source = request.configuration_source,
            },
            project_builder);
        builder.succeed(project_builder.build());
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_delete_node_instance(
    DeleteNodeInstanceRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_delete_node_instance(
            ProjectDeleteNodeInstanceRequest{.instance_id = request.instance_id},
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_update_node_instances(
    UpdateNodeInstancesRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        std::vector<ProjectUpdateNodeInstance> updates;
        updates.reserve(request.updates.size());
        for (auto const& update : request.updates) {
            updates.push_back(ProjectUpdateNodeInstance{
                .instance_id = update.instance_id,
                .display_name = update.display_name,
                .configuration_source = update.configuration_source,
            });
        }
        ProjectAckBuilder project_builder;
        handle_project_update_node_instances(
            ProjectUpdateNodeInstancesRequest{.updates = std::move(updates)},
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_upsert_graph_connection(
    UpsertGraphConnectionRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_upsert_graph_connection(
            ProjectUpsertGraphConnectionRequest{.connection = request.connection},
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}

void ProjectGraph::handle_socket_rpc_delete_graph_connection(
    DeleteGraphConnectionRequest const& request,
    SocketRpcAckResponseBuilder& builder)
{
    try {
        ProjectAckBuilder project_builder;
        handle_project_delete_graph_connection(
            ProjectDeleteGraphConnectionRequest{
                .connection_id = request.connection_id,
            },
            project_builder);
        project_builder.build();
        builder.succeed();
    } catch (std::exception const& error) {
        builder.fail(error.what());
    }
}
} // namespace iv
