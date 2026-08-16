#include <intravenous/runtime/graph_input_lanes/block_store.h>

namespace iv {
void GraphInputLanesBlockStore::publish_sample(LaneId lane, BorrowedSampleBlock const& block) {
  std::scoped_lock lock(_mutex);
  _sample_blocks[lane] = copy_sample_block(block.view());
}
void GraphInputLanesBlockStore::publish_event(LaneId lane, std::span<TimedEvent const> events) {
  std::scoped_lock lock(_mutex);
  auto& stored = _event_blocks[lane];
  stored.assign(events.begin(), events.end());
}
OwnedSampleBlock GraphInputLanesBlockStore::sample(LaneId lane) const {
  std::scoped_lock lock(_mutex);
  return _sample_blocks.contains(lane) ? _sample_blocks.at(lane) : OwnedSampleBlock{};
}
std::vector<TimedEvent> GraphInputLanesBlockStore::event(LaneId lane) const {
  std::scoped_lock lock(_mutex);
  return _event_blocks.contains(lane) ? _event_blocks.at(lane) : std::vector<TimedEvent>{};
}
} // namespace iv
