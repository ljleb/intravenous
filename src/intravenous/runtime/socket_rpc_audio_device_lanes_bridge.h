#pragma once

#include <intravenous/bridge.h>

namespace iv {
class AudioDeviceLanes;
class SocketRpcServer;

IV_DECLARE_BRIDGE(
    socket_rpc_audio_device_lanes_bridge,
    SocketRpcServer,
    AudioDeviceLanes);
} // namespace iv
