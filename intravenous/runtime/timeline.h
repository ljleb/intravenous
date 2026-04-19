#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace iv {
    enum class TimelineInputKind : std::uint8_t {
        sample,
        event,
    };

    struct TimelineLogicalInputId {
        size_t ordinal = 0;

        bool operator==(TimelineLogicalInputId const&) const = default;
    };

    struct TimelineLogicalInputKey {
        std::string logical_node_id {};
        TimelineLogicalInputId input_id {};
        TimelineInputKind kind = TimelineInputKind::sample;

        bool operator==(TimelineLogicalInputKey const&) const = default;
    };

    class Timeline {
    public:
    };
}
