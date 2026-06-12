#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/task_runner.h>

namespace iv {
using IvModuleInstancesExecutionTasksChangedEvent =
    void (*)(TaskGraphUpdate const &);

IV_DECLARE_LINKER_EVENT(
    IvModuleInstancesExecutionTasksChangedEvent,
    iv_runtime_iv_module_instances_execution_tasks_changed_event);
}
