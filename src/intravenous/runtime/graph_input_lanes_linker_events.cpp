#include <intravenous/runtime/graph_input_lanes_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    GraphInputLanesTimelineBatchRequestedEvent,
    iv_runtime_graph_input_lanes_timeline_batch_requested_event);
IV_DEFINE_LINKER_EVENT(
    GraphInputLanesRuntimeDependenciesChangedEvent,
    iv_runtime_graph_input_lanes_runtime_dependencies_changed_event);
} // namespace iv
