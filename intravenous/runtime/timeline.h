#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace iv {
    class GraphBuilder;
    struct InputConfig;
    struct EventInputConfig;
    struct SamplePortRef;
    struct EventPortRef;

    class Timeline {
    public:
        SamplePortRef resolve_sample_input(
            GraphBuilder& builder,
            std::span<std::string const> logical_node_ids,
            size_t input_ordinal,
            InputConfig const& input_config
        );

        EventPortRef resolve_event_input(
            GraphBuilder& builder,
            std::span<std::string const> logical_node_ids,
            size_t input_ordinal,
            EventInputConfig const& input_config
        );
    };
}
