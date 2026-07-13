#pragma once

#include <intravenous/lane_node/channels.h>
#include <intravenous/lane_node/ui_state.h>
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
        SampleStreamLayout sample_layout = SampleStreamLayout::planar;
    };

    struct CompiledEventLaneInputConfig {
        std::string name {};
        EventTypeId event_type = EventTypeId::empty;
    };

    struct RealtimeSampleLaneInputConfig {
        std::string name {};
        Sample default_value = 0.0f;
        SampleStreamLayout sample_layout = SampleStreamLayout::planar;
    };

    struct RealtimeEventLaneInputConfig {
        std::string name {};
        EventTypeId event_type = EventTypeId::empty;
    };

    struct CompiledSampleLaneOutputConfig {
        std::string name {};
        SampleStreamLayout sample_layout = SampleStreamLayout::planar;
    };

    struct CompiledEventLaneOutputConfig {
        std::string name {};
        EventTypeId event_type = EventTypeId::empty;
    };

    struct RealtimeSampleLaneOutputConfig {
        std::string name {};
        SampleStreamLayout sample_layout = SampleStreamLayout::planar;
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

    inline std::optional<SampleStreamLayout> sample_stream_layout_for(
        LaneOutputConfig const& output)
    {
        return std::visit([](auto const& config) -> std::optional<SampleStreamLayout> {
            using Config = std::remove_cvref_t<decltype(config)>;
            if constexpr (
                std::same_as<Config, CompiledSampleLaneOutputConfig>
                || std::same_as<Config, RealtimeSampleLaneOutputConfig>) {
                return config.sample_layout;
            } else {
                return std::nullopt;
            }
        }, output);
    }

    inline std::optional<ChannelLayout> sample_channel_layout_for(
        LaneOutputConfig const& output,
        std::optional<ChannelTypeId> sample_channel_type)
    {
        auto const sample_layout = sample_stream_layout_for(output);
        if (!sample_layout.has_value()) {
            return std::nullopt;
        }
        return ChannelLayout{
            .channel_type = sample_channel_type.value_or(ChannelTypeId::stereo),
            .sample_layout = *sample_layout,
        };
    }

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

        // An optional, presentation-independent authored model. A lane that
        // declares a model type id must provide the complete state contract
        // below; ordinary lanes do not opt in and retain no UI-model overhead.
        template<typename LaneNode>
        concept has_static_lane_model_type_id = requires {
            { LaneNode::lane_model_type_id() } -> std::convertible_to<std::string_view>;
        };

        template<typename LaneNode>
        concept has_member_lane_model_type_id = requires(LaneNode const& node) {
            { node.lane_model_type_id() } -> std::convertible_to<std::string_view>;
        };

        template<typename LaneNode>
        concept has_lane_ui_state = requires(
            LaneNode& node,
            LaneNode const& const_node,
            LaneUiStateWrite const& write) {
            { node.take_lane_ui_state_dirty() } -> std::convertible_to<bool>;
            { const_node.snapshot_lane_ui_state() } -> std::same_as<LaneUiStateSnapshot>;
            { node.apply_lane_ui_state(write) } -> std::same_as<LaneUiStateApplyResult>;
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

    template<typename LaneNode>
    std::string_view get_lane_model_type_id(LaneNode const& node)
    {
        if constexpr (lane_node_details::has_static_lane_model_type_id<LaneNode>) {
            return LaneNode::lane_model_type_id();
        } else {
            static_assert(
                lane_node_details::has_member_lane_model_type_id<LaneNode>,
                "lane model must define lane_model_type_id()");
            return node.lane_model_type_id();
        }
    }
}
