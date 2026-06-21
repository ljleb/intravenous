#include <intravenous/runtime/socket_rpc_audio_device_lanes_bridge.h>

#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/socket_rpc_server.h>

namespace iv {
namespace {
AudioDeviceLanes *bound_audio_device_lanes = nullptr;

void handle_get_audio_devices(
    GetAudioDevicesRequest const &,
    SocketRpcAudioDevicesResultBuilder &builder)
{
    if (bound_audio_device_lanes == nullptr) {
        return;
    }
    builder.succeed(bound_audio_device_lanes->audio_devices_snapshot());
}

void handle_set_audio_devices(
    SetAudioDevicesRequest const &request,
    SocketRpcAudioDevicesResultBuilder &builder)
{
    if (bound_audio_device_lanes == nullptr) {
        return;
    }
    builder.succeed(bound_audio_device_lanes->set_selected_devices(
        request.output_device_id,
        request.input_device_id));
}

IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcGetAudioDevicesEvent,
    iv_socket_rpc_get_audio_devices_event,
    handle_get_audio_devices);
IV_SUBSCRIBE_LINKER_EVENT(
    SocketRpcSetAudioDevicesEvent,
    iv_socket_rpc_set_audio_devices_event,
    handle_set_audio_devices);
} // namespace

void bind_socket_rpc_audio_device_lanes_bridge(AudioDeviceLanes &audio_device_lanes)
{
    bound_audio_device_lanes = &audio_device_lanes;
}

void unbind_socket_rpc_audio_device_lanes_bridge(
    AudioDeviceLanes const &audio_device_lanes)
{
    if (bound_audio_device_lanes == &audio_device_lanes) {
        bound_audio_device_lanes = nullptr;
    }
}
} // namespace iv
