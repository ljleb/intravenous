#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/graph_executor.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/package_definitions.h>

namespace iv {
using ProjectGraphNodeInstancesRequestedEvent =
    void (*)(NodeInstancesProjectGraphRequest&);
using ProjectGraphConnectionsRequestedEvent =
    void (*)(GraphConnectionsProjectGraphRequest&);
using ProjectGraphSourceIntrospectionRequestedEvent =
    void (*)(IvModuleSourceIntrospectionProjectGraphRequest&);
using ProjectGraphGraphJitRequestedEvent =
    GraphJitCompileResult (*)(ProjectGraphGraphJitRequest const&);
using ProjectGraphGraphExecutorRequestedEvent =
    ProjectGraphGraphExecutorResult (*)(
        std::shared_ptr<CompiledGraph const> const&);
using ProjectGraphGraphExecutorControlsRequestedEvent =
    void (*)(ProjectGraphGraphExecutorControlsRequest&);
using ProjectGraphNodeInstancesReadEvent = void (*)(NodeInstancesProjectGraphRead&);
using ProjectGraphConnectionsReadEvent = void (*)(GraphConnectionsProjectGraphRead&);
using ProjectGraphPackagesReadEvent = void (*)(PackageDefinitionsProjectGraphRead&);
using ProjectGraphNodeInstancesInteractionRequestedEvent =
    void (*)(NodeInstancesInteractionRequest&);
using ProjectGraphConnectionsInteractionRequestedEvent =
    void (*)(GraphConnectionsInteractionRequest&);

IV_DECLARE_LINKER_EVENT(
    ProjectGraphNodeInstancesRequestedEvent,
    iv_runtime_project_graph_node_instances_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectGraphConnectionsRequestedEvent,
    iv_runtime_project_graph_connections_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectGraphSourceIntrospectionRequestedEvent,
    iv_runtime_project_graph_source_introspection_requested_event);
IV_DECLARE_SINGLETON_EVENT(
    ProjectGraphGraphJitRequestedEvent,
    iv_runtime_project_graph_graph_jit_requested_event);
IV_DECLARE_SINGLETON_EVENT(
    ProjectGraphGraphExecutorRequestedEvent,
    iv_runtime_project_graph_graph_executor_requested_event);
IV_DECLARE_SINGLETON_EVENT(
    ProjectGraphGraphExecutorControlsRequestedEvent,
    iv_runtime_project_graph_graph_executor_controls_requested_event);
// Reads of the desired instances and connections for one graph scope read.
IV_DECLARE_LINKER_EVENT(
    ProjectGraphNodeInstancesReadEvent,
    iv_runtime_project_graph_node_instances_read_event);
IV_DECLARE_LINKER_EVENT(
    ProjectGraphConnectionsReadEvent,
    iv_runtime_project_graph_connections_read_event);
IV_DECLARE_LINKER_EVENT(
    ProjectGraphPackagesReadEvent,
    iv_runtime_project_graph_packages_read_event);
// Opens the NodeInstances and GraphConnections transactions of one node-message
// interaction; see NodeInstancesInteraction and GraphConnectionsInteraction.
IV_DECLARE_LINKER_EVENT(
    ProjectGraphNodeInstancesInteractionRequestedEvent,
    iv_runtime_project_graph_node_instances_interaction_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectGraphConnectionsInteractionRequestedEvent,
    iv_runtime_project_graph_connections_interaction_requested_event);
} // namespace iv
