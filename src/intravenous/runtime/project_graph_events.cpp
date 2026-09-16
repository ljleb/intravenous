#include <intravenous/runtime/project_graph_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    ProjectGraphNodeInstancesRequestedEvent,
    iv_runtime_project_graph_node_instances_requested_event)
IV_DEFINE_LINKER_EVENT(
    ProjectGraphConnectionsRequestedEvent,
    iv_runtime_project_graph_connections_requested_event)
} // namespace iv
