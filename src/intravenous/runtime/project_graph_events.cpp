#include <intravenous/runtime/project_graph_events.h>

namespace iv {
namespace {
GraphJitCompileResult ignore_graph_jit_request(GraphJitCompileRequest const&)
{
    return {};
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
} // namespace iv
