#pragma once

#include <intravenous/lane_node/channels.h>
#include <intravenous/ports.h>
#include <intravenous/sample.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iv {
struct LaneVisualizationSeries {
    std::uint64_t lane_id = 0;
    std::string adapter_type = "samples"; // "samples" or "events"
    std::optional<ChannelTypeId> sample_channel_type {};
    SampleStreamLayout sample_layout = SampleStreamLayout::planar;
    size_t sample_frame_count = 0;
    std::vector<Sample::storage> samples {};
    std::vector<TimedEvent> events {};
};

struct LaneViewContentUpdate {
    std::string view_id {};
    std::vector<LaneVisualizationSeries> lanes {};
};
} // namespace iv
