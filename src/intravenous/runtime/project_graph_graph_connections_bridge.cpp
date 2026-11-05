#include <intravenous/runtime/project_graph_graph_connections_bridge.h>

#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/project_graph_events.h>

namespace iv {
IV_DEFINE_BRIDGE(project_graph_graph_connections_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    project_graph_graph_connections_bridge,
    iv_runtime_project_graph_connections_requested_event,
    &GraphConnections::handle_project_graph_transaction)
} // namespace iv
