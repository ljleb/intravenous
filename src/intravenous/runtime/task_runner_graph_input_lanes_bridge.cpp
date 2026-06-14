#include <intravenous/runtime/task_runner_graph_input_lanes_bridge.h>

#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/task_runner_events.h>

namespace iv {
namespace {
GraphInputLanes *bound_lanes = nullptr;

void handle_task_runner_pass_finished(TaskRunnerPassFinished const &finished)
{
    if (bound_lanes == nullptr) {
        return;
    }
    bound_lanes->handle_task_runner_pass_finished(finished);
}

IV_SUBSCRIBE_LINKER_EVENT(
    TaskRunnerPassFinishedEvent,
    iv_runtime_task_runner_pass_finished_event,
    handle_task_runner_pass_finished);
} // namespace

void bind_task_runner_graph_input_lanes_bridge(GraphInputLanes &lanes)
{
    bound_lanes = &lanes;
}

void unbind_task_runner_graph_input_lanes_bridge(GraphInputLanes const &lanes)
{
    if (bound_lanes == &lanes) {
        bound_lanes = nullptr;
    }
}
} // namespace iv
