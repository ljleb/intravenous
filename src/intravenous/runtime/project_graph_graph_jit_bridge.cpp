#include <intravenous/runtime/project_graph_graph_jit_bridge.h>

#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/project_graph_events.h>

namespace iv {
IV_DEFINE_BRIDGE(project_graph_graph_jit_bridge)
IV_SUBSCRIBE_SINGLETON_BRIDGE(
    project_graph_graph_jit_bridge,
    iv_runtime_project_graph_graph_jit_requested_event,
    &GraphJit::handle_project_graph_transaction)
} // namespace iv
