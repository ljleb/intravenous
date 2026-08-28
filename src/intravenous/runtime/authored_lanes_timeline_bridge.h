#pragma once

#include <intravenous/bridge.h>

namespace iv {
class AuthoredLanes;
class Timeline;

IV_DECLARE_BRIDGE(authored_lanes_timeline_bridge, AuthoredLanes, Timeline);
} // namespace iv
