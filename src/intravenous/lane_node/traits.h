#pragma once

#include "ports.h"

#include <concepts>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace iv {
    enum class LanePortDomain : std::uint8_t {
        compiled,
        realtime,
    };

    struct CompiledSampleLaneInputConfig {
        std::string name {};
        Sample default_value = 0.0f;
    };

    struct CompiledEventLaneInputConfig {
        std::string name {};
        EventTypeId event_type = EventTypeId::empty;
    };

    struct RealtimeSampleLaneInputConfig {
        std::string name {};
        Sample default_value = 0.0f;
    };

    struct RealtimeEventLaneInputConfig {
        std::string name {};
        EventTypeId event_type = EventTypeId::empty;
    };

    struct CompiledSampleLaneOutputConfig {
        std::string name {};
    };

    struct CompiledEventLaneOutputConfig {
        std::string name {};
        EventTypeId event_type = EventTypeId::empty;
    };

    struct RealtimeSampleLaneOutputConfig {
        std::string name {};
    };

    struct RealtimeEventLaneOutputConfig {
        std::string name {};
        EventTypeId event_type = EventTypeId::empty;
    };

    using LaneOutputConfig = std::variant<
        CompiledSampleLaneOutputConfig,
        CompiledEventLaneOutputConfig,
        RealtimeSampleLaneOutputConfig,
        RealtimeEventLaneOutputConfig
    >;

    namespace lane_node_details {
        template<typename LaneNode>
        concept has_compiled_sample_inputs = requires(LaneNode const& node)
        {
            std::begin(node.compiled_sample_inputs());
            std::end(node.compiled_sample_inputs());
        };

        template<typename LaneNode>
        concept has_compiled_event_inputs = requires(LaneNode const& node)
        {
            std::begin(node.compiled_event_inputs());
            std::end(node.compiled_event_inputs());
        };

        template<typename LaneNode>
        concept has_realtime_sample_inputs = requires(LaneNode const& node)
        {
            std::begin(node.realtime_sample_inputs());
            std::end(node.realtime_sample_inputs());
        };

        template<typename LaneNode>
        concept has_realtime_event_inputs = requires(LaneNode const& node)
        {
            std::begin(node.realtime_event_inputs());
            std::end(node.realtime_event_inputs());
        };

        template<typename LaneNode>
        concept has_output = requires(LaneNode const& node)
        {
            node.output();
        };

    }

    template<typename LaneNode>
    auto get_compiled_sample_lane_inputs(LaneNode const& node)
    {
        if constexpr (lane_node_details::has_compiled_sample_inputs<LaneNode>) {
            return node.compiled_sample_inputs();
        } else {
            return std::span<CompiledSampleLaneInputConfig const, 0> {};
        }
    }

    template<typename LaneNode>
    auto get_compiled_event_lane_inputs(LaneNode const& node)
    {
        if constexpr (lane_node_details::has_compiled_event_inputs<LaneNode>) {
            return node.compiled_event_inputs();
        } else {
            return std::span<CompiledEventLaneInputConfig const, 0> {};
        }
    }

    template<typename LaneNode>
    auto get_realtime_sample_lane_inputs(LaneNode const& node)
    {
        if constexpr (lane_node_details::has_realtime_sample_inputs<LaneNode>) {
            return node.realtime_sample_inputs();
        } else {
            return std::span<RealtimeSampleLaneInputConfig const, 0> {};
        }
    }

    template<typename LaneNode>
    auto get_realtime_event_lane_inputs(LaneNode const& node)
    {
        if constexpr (lane_node_details::has_realtime_event_inputs<LaneNode>) {
            return node.realtime_event_inputs();
        } else {
            return std::span<RealtimeEventLaneInputConfig const, 0> {};
        }
    }

    template<typename LaneNode>
    auto get_lane_output(LaneNode const& node)
    {
        if constexpr (lane_node_details::has_output<LaneNode>) {
            return node.output();
        } else {
            static_assert(lane_node_details::has_output<LaneNode>, "lane node must define output()");
        }
    }

    template<typename OutputConfig>
    LaneOutputConfig normalize_lane_output(OutputConfig output)
    {
        if constexpr (std::same_as<std::remove_cvref_t<OutputConfig>, LaneOutputConfig>) {
            return output;
        } else {
            return LaneOutputConfig { std::move(output) };
        }
    }

    template<typename LaneNode>
    using LaneOutputDeclaration = decltype(get_lane_output(std::declval<LaneNode const&>()));
}
