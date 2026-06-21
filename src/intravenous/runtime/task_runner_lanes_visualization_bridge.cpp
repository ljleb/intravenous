#include <intravenous/runtime/task_runner_lanes_visualization_bridge.h>

#include <intravenous/runtime/lanes_visualization.h>
#include <intravenous/runtime/task_runner_events.h>

namespace iv {
namespace {
LanesVisualization *bound_visualization = nullptr;

void handle_task_runner_after_pass(TasksRunnerAfterPass const &finished)
{
    if (bound_visualization == nullptr) {
        return;
    }
    bound_visualization->handle_task_runner_after_pass(finished);
}

IV_SUBSCRIBE_LINKER_EVENT(
    TasksRunnerAfterPassEvent,
    iv_runtime_task_runner_after_pass_event,
    handle_task_runner_after_pass);
} // namespace

void bind_task_runner_lanes_visualization_bridge(LanesVisualization &visualization)
{
    bound_visualization = &visualization;
}

void unbind_task_runner_lanes_visualization_bridge(
    LanesVisualization const &visualization)
{
    if (bound_visualization == &visualization) {
        bound_visualization = nullptr;
    }
}
} // namespace iv
