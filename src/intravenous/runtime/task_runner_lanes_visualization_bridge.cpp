#include <intravenous/runtime/task_runner_lanes_visualization_bridge.h>

#include <intravenous/runtime/lanes_visualization.h>
#include <intravenous/runtime/task_runner_events.h>

namespace iv {
IV_DEFINE_BRIDGE(task_runner_lanes_visualization_bridge)
IV_SUBSCRIBE_BRIDGE(
    task_runner_lanes_visualization_bridge,
    iv_runtime_task_runner_after_pass_event,
    &LanesVisualization::handle_task_runner_after_pass);
} // namespace iv
