#include <intravenous/runtime/graph_output_blocks.h>

namespace iv {
void GraphOutputBlocks::publish_sample_block(LaneId lane, std::span<Sample const> block)
{
    std::scoped_lock lock(mutex_);
    auto &stored = sample_blocks_[lane];
    stored.assign(block.begin(), block.end());
}

void GraphOutputBlocks::publish_event_block(LaneId lane, std::span<TimedEvent const> events)
{
    std::scoped_lock lock(mutex_);
    auto &stored = event_blocks_[lane];
    stored.assign(events.begin(), events.end());
}

std::span<Sample const> GraphOutputBlocks::sample_block(LaneId lane) const
{
    std::scoped_lock lock(mutex_);
    auto const it = sample_blocks_.find(lane);
    if (it == sample_blocks_.end()) {
        return {};
    }
    return it->second;
}

std::span<TimedEvent const> GraphOutputBlocks::event_block(LaneId lane) const
{
    std::scoped_lock lock(mutex_);
    auto const it = event_blocks_.find(lane);
    if (it == event_blocks_.end()) {
        return {};
    }
    return it->second;
}
} // namespace iv
