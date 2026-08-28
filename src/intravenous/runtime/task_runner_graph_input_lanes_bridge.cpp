#include <intravenous/runtime/task_runner_graph_input_lanes_bridge.h>

#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/task_runner_events.h>

namespace iv {
IV_DEFINE_BRIDGE(task_runner_graph_input_lanes_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    task_runner_graph_input_lanes_bridge,
    iv_runtime_task_runner_after_pass_event,
    &GraphInputLanes::handle_task_runner_after_pass);
} // namespace iv
