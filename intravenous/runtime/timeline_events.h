#pragma once

#include "linker_event.h"
#include "runtime/lane_graph.h"

#include <vector>

namespace iv {
    struct RuntimeTimelineLanesChanged {
        bool lane_set_changed = false;
        std::vector<LaneId> changed_lanes {};
    };

    using RuntimeTimelineLanesChangedEvent =
        void (*)(RuntimeTimelineLanesChanged const &);

    IV_DECLARE_LINKER_EVENT(
        RuntimeTimelineLanesChangedEvent,
        iv_runtime_timeline_lanes_changed_event);
} // namespace iv
