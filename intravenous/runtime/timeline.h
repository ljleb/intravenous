#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace iv {
    class GraphBuilder;
    struct InputConfig;
    struct EventInputConfig;
    struct SamplePortRef;
    struct EventPortRef;

    class TimelineAugmentation {
    public:
        SamplePortRef resolve_sample_input(
            GraphBuilder& builder,
            std::string_view logical_node_id,
            size_t input_ordinal,
            InputConfig const& input_config
        );

        EventPortRef resolve_event_input(
            GraphBuilder& builder,
            std::string_view logical_node_id,
            size_t input_ordinal,
            EventInputConfig const& input_config
        );
    };

    class Timeline {
    public:
        TimelineAugmentation begin_augmentation();
    };
}
