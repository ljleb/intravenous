#pragma once

#include <intravenous/ports.h>

#include <array>
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
        concept has_static_compiled_sample_inputs = requires {
            LaneNode::compiled_sample_inputs();
        };

        template<typename LaneNode>
        concept has_member_compiled_sample_inputs = requires(LaneNode const& node) {
            node.compiled_sample_inputs();
        };

        template<typename LaneNode>
        concept has_static_compiled_event_inputs = requires {
            LaneNode::compiled_event_inputs();
        };

        template<typename LaneNode>
        concept has_member_compiled_event_inputs = requires(LaneNode const& node) {
            node.compiled_event_inputs();
        };

        template<typename LaneNode>
        concept has_static_realtime_sample_inputs = requires {
            LaneNode::realtime_sample_inputs();
        };

        template<typename LaneNode>
        concept has_member_realtime_sample_inputs = requires(LaneNode const& node) {
            node.realtime_sample_inputs();
        };

        template<typename LaneNode>
        concept has_static_realtime_event_inputs = requires {
            LaneNode::realtime_event_inputs();
        };

        template<typename LaneNode>
        concept has_member_realtime_event_inputs = requires(LaneNode const& node) {
            node.realtime_event_inputs();
        };

        template<typename LaneNode>
        concept has_static_output = requires {
            LaneNode::output();
        };

        template<typename LaneNode>
        concept has_member_output = requires(LaneNode const& node) {
            node.output();
        };
    } // namespace lane_node_details

    template<typename LaneNode>
    auto get_compiled_sample_lane_inputs(LaneNode const& node)
    {
        if constexpr (lane_node_details::has_static_compiled_sample_inputs<LaneNode>) {
            return LaneNode::compiled_sample_inputs();
        } else if constexpr (lane_node_details::has_member_compiled_sample_inputs<LaneNode>) {
            return node.compiled_sample_inputs();
        } else {
            return std::span<CompiledSampleLaneInputConfig const, 0> {};
        }
    }

    template<typename LaneNode>
    auto get_compiled_event_lane_inputs(LaneNode const& node)
    {
        if constexpr (lane_node_details::has_static_compiled_event_inputs<LaneNode>) {
            return LaneNode::compiled_event_inputs();
        } else if constexpr (lane_node_details::has_member_compiled_event_inputs<LaneNode>) {
            return node.compiled_event_inputs();
        } else {
            return std::span<CompiledEventLaneInputConfig const, 0> {};
        }
    }

    template<typename LaneNode>
    auto get_realtime_sample_lane_inputs(LaneNode const& node)
    {
        if constexpr (lane_node_details::has_static_realtime_sample_inputs<LaneNode>) {
            return LaneNode::realtime_sample_inputs();
        } else if constexpr (lane_node_details::has_member_realtime_sample_inputs<LaneNode>) {
            return node.realtime_sample_inputs();
        } else {
            return std::span<RealtimeSampleLaneInputConfig const, 0> {};
        }
    }

    template<typename LaneNode>
    auto get_realtime_event_lane_inputs(LaneNode const& node)
    {
        if constexpr (lane_node_details::has_static_realtime_event_inputs<LaneNode>) {
            return LaneNode::realtime_event_inputs();
        } else if constexpr (lane_node_details::has_member_realtime_event_inputs<LaneNode>) {
            return node.realtime_event_inputs();
        } else {
            return std::span<RealtimeEventLaneInputConfig const, 0> {};
        }
    }

    template<typename LaneNode>
    auto get_lane_output(LaneNode const& node)
    {
        if constexpr (lane_node_details::has_static_output<LaneNode>) {
            return LaneNode::output();
        } else if constexpr (lane_node_details::has_member_output<LaneNode>) {
            return node.output();
        } else {
            static_assert(
                lane_node_details::has_static_output<LaneNode>
                    || lane_node_details::has_member_output<LaneNode>,
                "lane node must define output()");
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

    inline LanePortDomain lane_output_domain(LaneOutputConfig const& output)
    {
        return std::visit([](auto const& config) -> LanePortDomain {
            using Config = std::remove_cvref_t<decltype(config)>;
            if constexpr (
                std::same_as<Config, CompiledSampleLaneOutputConfig>
                || std::same_as<Config, CompiledEventLaneOutputConfig>
            ) {
                return LanePortDomain::compiled;
            } else {
                return LanePortDomain::realtime;
            }
        }, output);
    }

    template<typename LaneNode>
    LanePortDomain get_lane_output_domain(LaneNode const& node)
    {
        return lane_output_domain(normalize_lane_output(get_lane_output(node)));
    }

    template<typename LaneNode>
    using LaneOutputDeclaration = decltype(get_lane_output(std::declval<LaneNode const&>()));
}
