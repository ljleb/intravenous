#include <intravenous/runtime/socket_rpc_system_audio_devices_bridge.h>

#include <intravenous/runtime/socket_rpc_server.h>
#include <intravenous/runtime/system_audio_devices.h>

namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_system_audio_devices_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_system_audio_devices_bridge,
    iv_socket_rpc_get_audio_devices_event,
    &SystemAudioDevices::handle_socket_rpc_get_audio_devices)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_system_audio_devices_bridge,
    iv_socket_rpc_set_audio_devices_event,
    &SystemAudioDevices::handle_socket_rpc_set_audio_devices)
} // namespace iv
