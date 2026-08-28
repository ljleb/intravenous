#include <intravenous/runtime/socket_rpc_audio_device_lanes_bridge.h>

#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
IV_DEFINE_BRIDGE(socket_rpc_audio_device_lanes_bridge)

IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_audio_device_lanes_bridge,
    iv_socket_rpc_get_audio_devices_event,
    &AudioDeviceLanes::handle_socket_rpc_get_audio_devices)
IV_SUBSCRIBE_LINKER_EVENT(
    socket_rpc_audio_device_lanes_bridge,
    iv_socket_rpc_set_audio_devices_event,
    &AudioDeviceLanes::handle_socket_rpc_set_audio_devices)
} // namespace iv
