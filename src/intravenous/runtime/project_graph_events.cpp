#include <intravenous/runtime/project_graph_events.h>

namespace iv {
namespace {
GraphJitCompileResult ignore_graph_jit_request(ProjectGraphGraphJitRequest const&)
{
    return {};
}

ProjectGraphGraphExecutorResult ignore_graph_executor_request(
    std::shared_ptr<CompiledGraph const> const&)
{
    return {};
}

void ignore_graph_executor_controls_request(
    ProjectGraphGraphExecutorControlsRequest&)
{}
} // namespace
IV_DEFINE_LINKER_EVENT(
    ProjectGraphNodeInstancesRequestedEvent,
    iv_runtime_project_graph_node_instances_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphConnectionsRequestedEvent,
    iv_runtime_project_graph_connections_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphSourceIntrospectionRequestedEvent,
    iv_runtime_project_graph_source_introspection_requested_event)
IV_DEFINE_SINGLETON_EVENT(
    ProjectGraphGraphJitRequestedEvent,
    iv_runtime_project_graph_graph_jit_requested_event,
    &ignore_graph_jit_request)
IV_DEFINE_SINGLETON_EVENT(
    ProjectGraphGraphExecutorRequestedEvent,
    iv_runtime_project_graph_graph_executor_requested_event,
    &ignore_graph_executor_request)
IV_DEFINE_SINGLETON_EVENT(
    ProjectGraphGraphExecutorControlsRequestedEvent,
    iv_runtime_project_graph_graph_executor_controls_requested_event,
    &ignore_graph_executor_controls_request)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphNodeInstancesReadEvent,
    iv_runtime_project_graph_node_instances_read_event)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphConnectionsReadEvent,
    iv_runtime_project_graph_connections_read_event)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphNodeInstancesInteractionRequestedEvent,
    iv_runtime_project_graph_node_instances_interaction_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphConnectionsInteractionRequestedEvent,
    iv_runtime_project_graph_connections_interaction_requested_event)
} // namespace iv
