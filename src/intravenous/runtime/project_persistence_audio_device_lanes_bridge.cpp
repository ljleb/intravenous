#include <intravenous/runtime/project_persistence_audio_device_lanes_bridge.h>

#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/runtime_project_events.h>

namespace iv {
IV_DEFINE_BRIDGE(project_persistence_audio_device_lanes_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_audio_device_lanes_bridge,
    iv_runtime_project_set_audio_devices_requested_event,
    &AudioDeviceLanes::handle_project_set_audio_devices)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_audio_device_lanes_bridge,
    iv_runtime_project_set_audio_device_lane_ids_requested_event,
    &AudioDeviceLanes::handle_project_set_audio_device_lane_ids)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_audio_device_lanes_bridge,
    iv_runtime_project_override_settings_requested_event,
    &AudioDeviceLanes::handle_project_override_settings)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_audio_device_lanes_bridge,
    iv_runtime_project_persistence_collect_state_event,
    &AudioDeviceLanes::handle_project_persistence_collect_state)
} // namespace iv
