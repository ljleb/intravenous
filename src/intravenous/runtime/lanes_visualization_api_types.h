#pragma once

#include <intravenous/lane_node/channels.h>
#include <intravenous/ports.h>
#include <intravenous/sample.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <intravenous/runtime/uuid.h>

namespace iv {
struct LaneVisualizationSeries {
    InternedString lane_id {};
    std::string adapter_type = "level"; // "level", "activity", or "events"
    std::optional<ChannelTypeId> sample_channel_type {};
    // Sample lanes contribute exactly one level value per visible lane and UI
    // refresh. Raw sample buffers are deliberately not part of this API.
    std::optional<Sample::storage> peak_level {};
    std::optional<Sample::storage> secondary_peak_level {};
    std::optional<size_t> event_count {};
    std::vector<TimedEvent> events {};
};

struct LaneViewContentUpdate {
    InternedString view_id {};
    std::optional<size_t> playback_sample_index {};
    std::vector<LaneVisualizationSeries> lanes {};
};
} // namespace iv
