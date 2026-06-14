#pragma once

#include <intravenous/runtime/lane_graph.h>

#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace iv {
// Small store mirroring TimelineExecution's realtime block service, but inverted:
// DSP output sink nodes publish blocks here, and timeline output-lane nodes read
// them back. Producer (DSP task) and consumer (lane task) are ordered by the task
// graph (the lane task depends_on the DSP task), so there is no same-lane data race
// within a pass.
class GraphOutputBlocks {
    mutable std::mutex mutex_;
    std::unordered_map<LaneId, std::vector<Sample>, LaneIdHash> sample_blocks_;
    std::unordered_map<LaneId, std::vector<TimedEvent>, LaneIdHash> event_blocks_;

public:
    void publish_sample_block(LaneId lane, std::span<Sample const> block);
    void publish_event_block(LaneId lane, std::span<TimedEvent const> events);
    [[nodiscard]] std::span<Sample const> sample_block(LaneId lane) const;
    [[nodiscard]] std::span<TimedEvent const> event_block(LaneId lane) const;
};
} // namespace iv
