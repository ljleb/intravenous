#pragma once

#include <intravenous/bridge.h>

namespace iv {
class ConfiguredLanes;
class Timeline;

IV_DECLARE_BRIDGE(configured_lanes_timeline_bridge, ConfiguredLanes, Timeline);
} // namespace iv
