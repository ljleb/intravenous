#pragma once

#include <intravenous/bridge.h>

namespace iv {
class SocketRpcServer;
class SystemAudioDevices;

IV_DECLARE_BRIDGE(
    socket_rpc_system_audio_devices_bridge,
    SocketRpcServer,
    SystemAudioDevices);
} // namespace iv
