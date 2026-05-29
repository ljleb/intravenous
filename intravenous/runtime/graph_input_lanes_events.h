#pragma once

#include "linker_event.h"
#include "runtime/timeline_events.h"

#include <optional>
#include <string>

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

IV_DECLARE_LINKER_EVENT(
    GraphInputLanesTimelineBatchRequestedEvent,
    iv_runtime_graph_input_lanes_timeline_batch_requested_event);
} // namespace iv
