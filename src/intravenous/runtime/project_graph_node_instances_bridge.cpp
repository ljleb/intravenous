#include <intravenous/runtime/project_graph_node_instances_bridge.h>

#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/project_graph_events.h>

namespace iv {
IV_DEFINE_BRIDGE(project_graph_node_instances_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    project_graph_node_instances_bridge,
    iv_runtime_project_graph_node_instances_requested_event,
    &NodeInstances::handle_project_graph_transaction)
} // namespace iv
