#include <intravenous/runtime/node_definitions_project_graph_bridge.h>

#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/project_graph.h>

namespace iv {
IV_DEFINE_BRIDGE(node_definitions_project_graph_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    node_definitions_project_graph_bridge,
    iv_runtime_node_definitions_snapshot_changed_event,
    &ProjectGraph::handle_node_definitions_snapshot_changed)
} // namespace iv
