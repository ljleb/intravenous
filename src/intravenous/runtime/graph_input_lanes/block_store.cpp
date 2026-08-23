#include <intravenous/runtime/graph_input_lanes/block_store.h>

namespace iv {
void GraphInputLanesBlockStore::prepare_sample(LaneId lane) {
  std::scoped_lock lock(_mutex);
  auto [it, _] = _sample_blocks.try_emplace(lane);
  it->second = {};
}
void GraphInputLanesBlockStore::prepare_event(LaneId lane) {
  std::scoped_lock lock(_mutex);
  auto [it, _] = _event_blocks.try_emplace(lane);
  it->second = {};
}
void GraphInputLanesBlockStore::publish_sample(LaneId lane, BorrowedSampleBlock const& block) {
  std::scoped_lock lock(_mutex);
  auto const it = _sample_blocks.find(lane);
  if (it != _sample_blocks.end()) it->second = block;
}
void GraphInputLanesBlockStore::publish_event(LaneId lane, std::span<TimedEvent const> events) {
  std::scoped_lock lock(_mutex);
  auto const it = _event_blocks.find(lane);
  if (it != _event_blocks.end()) it->second = events;
}
BorrowedSampleBlock GraphInputLanesBlockStore::sample(LaneId lane) const {
  std::scoped_lock lock(_mutex);
  auto const it = _sample_blocks.find(lane);
  return it != _sample_blocks.end() ? it->second : BorrowedSampleBlock{};
}
std::span<TimedEvent const> GraphInputLanesBlockStore::event(LaneId lane) const {
  std::scoped_lock lock(_mutex);
  auto const it = _event_blocks.find(lane);
  return it != _event_blocks.end() ? it->second : std::span<TimedEvent const>{};
}
} // namespace iv
