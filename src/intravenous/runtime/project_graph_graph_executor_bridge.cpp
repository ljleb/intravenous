#include <intravenous/runtime/project_graph_graph_executor_bridge.h>

#include <intravenous/runtime/graph_executor.h>
#include <intravenous/runtime/project_graph_events.h>

namespace iv {
IV_DEFINE_BRIDGE(project_graph_graph_executor_bridge)
IV_SUBSCRIBE_SINGLETON_BRIDGE(
    project_graph_graph_executor_bridge,
    iv_runtime_project_graph_graph_executor_requested_event,
    &GraphExecutor::handle_project_graph_transaction)
IV_SUBSCRIBE_SINGLETON_BRIDGE(
    project_graph_graph_executor_bridge,
    iv_runtime_project_graph_realtime_code_requested_event,
    &GraphExecutor::stage_realtime_code)
IV_SUBSCRIBE_SINGLETON_BRIDGE(
    project_graph_graph_executor_bridge,
    iv_runtime_project_graph_manual_control_target_requested_event,
    &GraphExecutor::set_manual_control_target)
} // namespace iv
