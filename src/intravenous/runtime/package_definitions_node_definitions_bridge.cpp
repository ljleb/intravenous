#include <intravenous/runtime/package_definitions_node_definitions_bridge.h>
#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/package_pipeline_events.h>
namespace iv {
IV_DEFINE_BRIDGE(package_definitions_node_definitions_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    package_definitions_node_definitions_bridge,
    iv_runtime_package_definitions_publication_requested_event,
    &NodeDefinitions::handle_package_definitions_publication)
}
