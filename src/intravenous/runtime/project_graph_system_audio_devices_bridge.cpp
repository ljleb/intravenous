#include <intravenous/runtime/project_graph_system_audio_devices_bridge.h>
#include <intravenous/runtime/system_audio_devices.h>
#include <intravenous/runtime/project_graph_events.h>
namespace iv {
IV_DEFINE_BRIDGE(project_graph_system_audio_devices_bridge)
IV_SUBSCRIBE_LINKER_EVENT(project_graph_system_audio_devices_bridge, iv_runtime_project_graph_audio_selection_requested_event,
    &SystemAudioDevices::handle_project_graph_audio_selection)
} // namespace iv
