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
  void publish_sample(LaneId, BorrowedSampleBlock const&);
  void publish_event(LaneId, std::span<TimedEvent const>);
  OwnedSampleBlock sample(LaneId) const;
  std::vector<TimedEvent> event(LaneId) const;

private:
  mutable std::mutex _mutex;
  std::unordered_map<LaneId, OwnedSampleBlock, LaneIdHash> _sample_blocks;
  std::unordered_map<LaneId, std::vector<TimedEvent>, LaneIdHash> _event_blocks;
};
} // namespace iv
