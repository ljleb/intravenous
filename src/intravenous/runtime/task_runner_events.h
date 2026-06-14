#pragma once

#include <intravenous/linker_event.h>

#include <cstdint>

namespace iv {
struct TaskRunnerPassFinished {
    std::uint64_t graph_revision = 0;
};

using TaskRunnerPassFinishedEvent =
    void (*)(TaskRunnerPassFinished const &);

IV_DECLARE_LINKER_EVENT(
    TaskRunnerPassFinishedEvent,
    iv_runtime_task_runner_pass_finished_event);
} // namespace iv
