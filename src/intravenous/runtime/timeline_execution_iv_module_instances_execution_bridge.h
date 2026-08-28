#pragma once

#include <intravenous/bridge.h>

namespace iv {
class TimelineExecution;
class IvModuleInstancesExecution;

IV_DECLARE_BRIDGE(
    timeline_execution_iv_module_instances_execution_bridge,
    TimelineExecution,
    IvModuleInstancesExecution);
} // namespace iv
