#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/timeline_events.h>

#include <optional>
#include <string>
#include <vector>

namespace iv {
class GraphInputLanesAckBuilder {
    std::optional<std::string> error_message;
    bool handled = false;

public:
    void succeed();
    void fail(std::string message);
    void build() const;
};

using GraphInputLanesTimelineBatchRequestedEvent =
    void (*)(TimelineLaneBatchUpdate const &, GraphInputLanesAckBuilder &);

struct GraphInputLanesRebuildRequested {
    std::vector<std::string> instance_ids {};
};

using GraphInputLanesRebuildRequestedEvent =
    void (*)(GraphInputLanesRebuildRequested const &);

IV_DECLARE_LINKER_EVENT(
    GraphInputLanesTimelineBatchRequestedEvent,
    iv_runtime_graph_input_lanes_timeline_batch_requested_event);
IV_DECLARE_LINKER_EVENT(
    GraphInputLanesRebuildRequestedEvent,
    iv_runtime_graph_input_lanes_rebuild_requested_event);
} // namespace iv
