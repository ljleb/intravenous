#include <intravenous/runtime/task_runner_lanes_visualization_bridge.h>

#include <intravenous/runtime/lanes_visualization.h>
#include <intravenous/runtime/task_runner_events.h>

namespace iv {
namespace {
LanesVisualization *bound_visualization = nullptr;

void handle_task_runner_pass_finished(TasksRunnerPassFinished const &finished)
{
    if (bound_visualization == nullptr) {
        return;
    }
    bound_visualization->handle_task_runner_pass_finished(finished);
}

IV_SUBSCRIBE_LINKER_EVENT(
    TasksRunnerPassFinishedEvent,
    iv_runtime_task_runner_pass_finished_event,
    handle_task_runner_pass_finished);
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
