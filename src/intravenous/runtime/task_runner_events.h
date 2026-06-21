#pragma once

#include <intravenous/linker_event.h>

#include <cstdint>

namespace iv {
struct TasksRunnerPassFinished {
    std::uint64_t graph_revision = 0;
};

using TasksRunnerPassFinishedEvent =
    void (*)(TasksRunnerPassFinished const &);

IV_DECLARE_LINKER_EVENT(
    TasksRunnerPassFinishedEvent,
    iv_runtime_task_runner_pass_finished_event);
} // namespace iv
