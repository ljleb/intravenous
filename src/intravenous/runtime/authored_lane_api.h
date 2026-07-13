#pragma once

#include <intravenous/runtime/uuid.h>
#include <intravenous/lane_node/graph.h>

#include <cstddef>
#include <string>

namespace iv {
struct LaneCreationContext {
    size_t sample_rate = 48000;
};

struct CreatableLaneDescriptor {
    std::string type_id;
    std::string category;
    std::string label;
    std::string description;
};

struct AuthoredLaneRecord {
    InternedString lane_id;
    std::string type_id;
    std::string serialized_state;
};

// Explicit user wiring.  Generated graph wiring is intentionally not an
// authored record and is never written to a project file.
struct AuthoredLaneConnection {
    InternedString source_lane_id;
    InternedString target_lane_id;
    LanePortId input;
};
} // namespace iv
