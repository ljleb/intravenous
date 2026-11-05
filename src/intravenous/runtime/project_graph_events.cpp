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

GraphExecutorStageResult ignore_realtime_code_request(
    std::shared_ptr<CompiledGraph const>)
{
    return GraphExecutorStageResult::ignored_stale;
}

bool ignore_manual_control_target_request(
    std::uint64_t, ManualControlId const&, Sample)
{
    return false;
}
} // namespace
IV_DEFINE_LINKER_EVENT(
    ProjectGraphNodeInstancesRequestedEvent,
    iv_runtime_project_graph_node_instances_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphConnectionsRequestedEvent,
    iv_runtime_project_graph_connections_requested_event)
IV_DEFINE_SINGLETON_EVENT(
    ProjectGraphGraphJitRequestedEvent,
    iv_runtime_project_graph_graph_jit_requested_event,
    &ignore_graph_jit_request)
IV_DEFINE_SINGLETON_EVENT(
    ProjectGraphGraphExecutorRequestedEvent,
    iv_runtime_project_graph_graph_executor_requested_event,
    &ignore_graph_executor_request)
IV_DEFINE_SINGLETON_EVENT(
    ProjectGraphRealtimeCodeRequestedEvent,
    iv_runtime_project_graph_realtime_code_requested_event,
    &ignore_realtime_code_request)
IV_DEFINE_SINGLETON_EVENT(
    ProjectGraphManualControlTargetRequestedEvent,
    iv_runtime_project_graph_manual_control_target_requested_event,
    &ignore_manual_control_target_request)
} // namespace iv
