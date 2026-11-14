#include <intravenous/runtime/project_graph_package_definitions_bridge.h>
#include <intravenous/runtime/package_definitions.h>
#include <intravenous/runtime/project_graph_events.h>

namespace iv {
IV_DEFINE_BRIDGE(project_graph_package_definitions_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    project_graph_package_definitions_bridge,
    iv_runtime_project_graph_packages_read_event,
    &PackageDefinitions::handle_project_graph_read)
} // namespace iv
