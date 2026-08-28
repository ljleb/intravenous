#pragma once

#include <intravenous/bridge.h>

namespace iv {
class AudioDeviceLanes;
class IvModuleInstancesExecution;

IV_DECLARE_BRIDGE(
    audio_device_lanes_iv_module_instances_execution_bridge,
    AudioDeviceLanes,
    IvModuleInstancesExecution);
} // namespace iv
