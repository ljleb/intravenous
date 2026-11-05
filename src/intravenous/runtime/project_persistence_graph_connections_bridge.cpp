#include <intravenous/runtime/project_persistence_graph_connections_bridge.h>

#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/project_persistence_events.h>

namespace iv {
IV_DEFINE_BRIDGE(project_persistence_graph_connections_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_graph_connections_bridge,
    iv_runtime_project_persistence_collect_state_event,
    &GraphConnections::handle_project_persistence_collect_state)
} // namespace iv
