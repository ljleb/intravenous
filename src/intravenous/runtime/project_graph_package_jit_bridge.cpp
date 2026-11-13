#include <intravenous/runtime/project_graph_package_jit_bridge.h>
#include <intravenous/runtime/package_jit.h>
#include <intravenous/runtime/project_graph_events.h>
namespace iv {
IV_DEFINE_BRIDGE(project_graph_package_jit_bridge)
IV_SUBSCRIBE_LINKER_EVENT(project_graph_package_jit_bridge, iv_runtime_project_graph_toolchain_requested_event,
    &PackageJit::handle_project_graph_toolchain)
} // namespace iv
