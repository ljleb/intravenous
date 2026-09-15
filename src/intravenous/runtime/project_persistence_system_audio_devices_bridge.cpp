#include <intravenous/runtime/project_persistence_system_audio_devices_bridge.h>

#include <intravenous/runtime/project_persistence_events.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/system_audio_devices.h>

namespace iv {
IV_DEFINE_BRIDGE(project_persistence_system_audio_devices_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_system_audio_devices_bridge,
    iv_runtime_project_set_audio_devices_requested_event,
    &SystemAudioDevices::handle_project_set_audio_devices)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_system_audio_devices_bridge,
    iv_runtime_project_override_settings_requested_event,
    &SystemAudioDevices::handle_project_override_settings)
IV_SUBSCRIBE_LINKER_EVENT(
    project_persistence_system_audio_devices_bridge,
    iv_runtime_project_persistence_collect_state_event,
    &SystemAudioDevices::handle_project_persistence_collect_state)
} // namespace iv
