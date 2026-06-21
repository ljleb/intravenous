#pragma once

#include <intravenous/linker_event.h>

#include <cstdint>

namespace iv {
struct TasksRunnerBeforePass {
    std::uint64_t graph_revision = 0;
};

struct TasksRunnerAfterPass {
    std::uint64_t graph_revision = 0;
};

using TasksRunnerBeforePassEvent =
    void (*)(TasksRunnerBeforePass const &);
using TasksRunnerAfterPassEvent =
    void (*)(TasksRunnerAfterPass const &);

IV_DECLARE_LINKER_EVENT(
    TasksRunnerBeforePassEvent,
    iv_runtime_task_runner_before_pass_event);
IV_DECLARE_LINKER_EVENT(
    TasksRunnerAfterPassEvent,
    iv_runtime_task_runner_after_pass_event);
} // namespace iv
