#include <intravenous/app/bridges/project_graph_graph_executor_bridge.h>

#include <intravenous/execution/graph_executor.h>
#include <intravenous/project/project_graph_events.h>

namespace iv {
IV_DEFINE_BRIDGE(project_graph_graph_executor_bridge)
IV_SUBSCRIBE_LINKER_EVENT(project_graph_graph_executor_bridge,
    iv_runtime_project_graph_execution_read_event, &GraphExecutor::handle_execution_read)
IV_SUBSCRIBE_SINGLETON_BRIDGE(
    project_graph_graph_executor_bridge,
    iv_runtime_project_graph_graph_executor_requested_event,
    &GraphExecutor::handle_project_graph_transaction)
IV_SUBSCRIBE_SINGLETON_BRIDGE(
    project_graph_graph_executor_bridge,
    iv_runtime_project_graph_execution_recovery_event,
    &GraphExecutor::reject_failed_pending_generation)
IV_SUBSCRIBE_SINGLETON_BRIDGE(
    project_graph_graph_executor_bridge,
    iv_runtime_project_graph_graph_executor_controls_requested_event,
    &GraphExecutor::handle_project_graph_controls)
IV_SUBSCRIBE_SINGLETON_BRIDGE(
    project_graph_graph_executor_bridge,
    iv_runtime_project_graph_realtime_observation_requested_event,
    &GraphExecutor::stage_observation_update)
} // namespace iv
