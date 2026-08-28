#include <intravenous/runtime/audio_device_lanes_iv_module_instances_execution_bridge.h>

#include <intravenous/runtime/audio_device_lanes_events.h>
#include <intravenous/runtime/iv_module_instances_execution.h>

namespace iv {
IV_DEFINE_BRIDGE(audio_device_lanes_iv_module_instances_execution_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    audio_device_lanes_iv_module_instances_execution_bridge,
    iv_runtime_audio_device_lanes_set_realtime_start_index_event,
    &IvModuleInstancesExecution::synchronize_transport_playhead);
} // namespace iv
