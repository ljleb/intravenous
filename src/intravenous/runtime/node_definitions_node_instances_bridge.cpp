#include <intravenous/runtime/node_definitions_node_instances_bridge.h>

#include <intravenous/runtime/node_definitions_events.h>
#include <intravenous/runtime/node_instances.h>

namespace iv {
IV_DEFINE_BRIDGE(node_definitions_node_instances_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    node_definitions_node_instances_bridge,
    iv_runtime_node_definitions_snapshot_changed_event,
    &NodeInstances::handle_node_definitions_snapshot_changed);
} // namespace iv
