#pragma once

#include <intravenous/ports.h>
#include <intravenous/sample.h>

#include <cstdint>
#include <string>
#include <vector>

namespace iv {
struct LaneVisualizationSeries {
    std::uint64_t lane_id = 0;
    std::string adapter_type = "samples"; // "samples" or "events"
    std::vector<Sample::storage> samples {};
    std::vector<TimedEvent> events {};
};

struct LaneViewContentUpdate {
    std::string view_id {};
    std::vector<LaneVisualizationSeries> lanes {};
};
} // namespace iv
