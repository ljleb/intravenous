#include <intravenous/runtime/graph_input_lanes_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    GraphInputLanesTimelineBatchRequestedEvent,
    iv_runtime_graph_input_lanes_timeline_batch_requested_event);
IV_DEFINE_LINKER_EVENT(
    GraphInputLanesRebuildRequestedEvent,
    iv_runtime_graph_input_lanes_rebuild_requested_event);
} // namespace iv
