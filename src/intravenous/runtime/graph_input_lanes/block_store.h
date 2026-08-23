#pragma once

#include <intravenous/runtime/sample_stream_blocks.h>
#include <intravenous/runtime/lane_graph.h>

#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace iv {
class GraphInputLanesBlockStore {
public:
  void prepare_sample(LaneId);
  void prepare_event(LaneId);
  void publish_sample(LaneId, BorrowedSampleBlock const&);
  void publish_event(LaneId, std::span<TimedEvent const>);
  BorrowedSampleBlock sample(LaneId) const;
  std::span<TimedEvent const> event(LaneId) const;

private:
  mutable std::mutex _mutex;
  std::unordered_map<LaneId, BorrowedSampleBlock, LaneIdHash> _sample_blocks;
  std::unordered_map<LaneId, std::span<TimedEvent const>, LaneIdHash> _event_blocks;
};
} // namespace iv
