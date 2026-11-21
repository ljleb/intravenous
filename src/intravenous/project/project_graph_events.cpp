#include <intravenous/project/project_graph_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(ProjectGraphExecutionReadEvent, iv_runtime_project_graph_execution_read_event)
IV_DEFINE_LINKER_EVENT(ProjectGraphExecutionPublishedEvent, iv_runtime_project_graph_execution_published_event)
namespace {
GraphJitCompileResult ignore_graph_jit_request(ProjectGraphGraphJitRequest const&)
{
    return {};
}

ProjectGraphGraphExecutorResult ignore_graph_executor_request(
    ProjectGraphGraphExecutorRequest const&)
{
    return {};
}

std::optional<FailedExecutionGeneration> ignore_execution_recovery()
{
    return std::nullopt;
}

void ignore_graph_executor_controls_request(
    ProjectGraphGraphExecutorControlsRequest&)
{}

bool ignore_realtime_observation_request(ProjectGraphRealtimeObservationRequest const&)
{
    return false;
}
} // namespace
IV_DEFINE_LINKER_EVENT(ProjectGraphAudioSelectionRequestedEvent, iv_runtime_project_graph_audio_selection_requested_event)
IV_DEFINE_LINKER_EVENT(ProjectGraphToolchainRequestedEvent, iv_runtime_project_graph_toolchain_requested_event)
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
    ProjectGraphExecutionRecoveryEvent,
    iv_runtime_project_graph_execution_recovery_event,
    &ignore_execution_recovery)
IV_DEFINE_SINGLETON_EVENT(
    ProjectGraphGraphExecutorControlsRequestedEvent,
    iv_runtime_project_graph_graph_executor_controls_requested_event,
    &ignore_graph_executor_controls_request)
IV_DEFINE_SINGLETON_EVENT(
    ProjectGraphRealtimeObservationRequestedEvent,
    iv_runtime_project_graph_realtime_observation_requested_event,
    &ignore_realtime_observation_request)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphNodeInstancesReadEvent,
    iv_runtime_project_graph_node_instances_read_event)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphConnectionsReadEvent,
    iv_runtime_project_graph_connections_read_event)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphPackagesReadEvent,
    iv_runtime_project_graph_packages_read_event)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphNodeInstancesInteractionRequestedEvent,
    iv_runtime_project_graph_node_instances_interaction_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphConnectionsInteractionRequestedEvent,
    iv_runtime_project_graph_connections_interaction_requested_event)
} // namespace iv
