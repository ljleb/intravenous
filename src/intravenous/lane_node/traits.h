#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/lane_node/ui_state.h>
#include <intravenous/ports.h>

#include <array>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace iv {
    enum class LanePortDomain : std::uint8_t {
        compiled,
        realtime,
    };

    struct LaneSampleInputPortConfig {
        ChannelLayout channel_layout {};
        Sample default_value = 0.0f;
    };

    struct LaneEventInputPortConfig {
        EventTypeId event_type = EventTypeId::empty;
    };

    struct LaneInputConfig {
        std::string name {};
        LanePortDomain domain = LanePortDomain::realtime;
        std::variant<LaneSampleInputPortConfig, LaneEventInputPortConfig> kind_config;
    };

    struct LaneSampleOutputPortConfig {
        ChannelLayout channel_layout {};
    };

    struct LaneEventOutputPortConfig {
        EventTypeId event_type = EventTypeId::empty;
    };

    struct LaneOutputConfig {
        std::string name {};
        LanePortDomain domain = LanePortDomain::realtime;
        std::variant<LaneSampleOutputPortConfig, LaneEventOutputPortConfig> kind_config;
    };

    inline PortKind lane_input_kind(LaneInputConfig const& input)
    {
        return std::holds_alternative<LaneSampleInputPortConfig>(input.kind_config)
            ? PortKind::sample
            : PortKind::event;
    }

    inline PortKind lane_output_kind(LaneOutputConfig const& output)
    {
        return std::holds_alternative<LaneSampleOutputPortConfig>(output.kind_config)
            ? PortKind::sample
            : PortKind::event;
    }

    inline std::optional<ChannelLayout> sample_channel_layout_for(LaneInputConfig const& input)
    {
        if (auto const* sample = std::get_if<LaneSampleInputPortConfig>(&input.kind_config)) {
            return sample->channel_layout;
        }
        return std::nullopt;
    }

    inline std::optional<ChannelLayout> sample_channel_layout_for(LaneOutputConfig const& output)
    {
        if (auto const* sample = std::get_if<LaneSampleOutputPortConfig>(&output.kind_config)) {
            return sample->channel_layout;
        }
        return std::nullopt;
    }

    inline std::optional<EventTypeId> event_type_for(LaneInputConfig const& input)
    {
        if (auto const* event = std::get_if<LaneEventInputPortConfig>(&input.kind_config)) {
            return event->event_type;
        }
        return std::nullopt;
    }

    inline std::optional<EventTypeId> event_type_for(LaneOutputConfig const& output)
    {
        if (auto const* event = std::get_if<LaneEventOutputPortConfig>(&output.kind_config)) {
            return event->event_type;
        }
        return std::nullopt;
    }

    inline bool lane_port_matches(
        LaneInputConfig const& input,
        LanePortDomain domain,
        PortKind kind)
    {
        return input.domain == domain && lane_input_kind(input) == kind;
    }

    inline bool lane_port_matches(
        LaneOutputConfig const& output,
        LanePortDomain domain,
        PortKind kind)
    {
        return output.domain == domain && lane_output_kind(output) == kind;
    }

    namespace lane_node_details {
        template<typename LaneNode>
        concept has_static_inputs = requires {
            LaneNode::inputs();
        };

        template<typename LaneNode>
        concept has_member_inputs = requires(LaneNode const& node) {
            node.inputs();
        };

        template<typename LaneNode>
        concept has_static_outputs = requires {
            LaneNode::outputs();
        };

        template<typename LaneNode>
        concept has_member_outputs = requires(LaneNode const& node) {
            node.outputs();
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
    auto get_lane_inputs(LaneNode const& node)
    {
        if constexpr (lane_node_details::has_static_inputs<LaneNode>) {
            return LaneNode::inputs();
        } else if constexpr (lane_node_details::has_member_inputs<LaneNode>) {
            return node.inputs();
        } else {
            return std::span<LaneInputConfig const, 0> {};
        }
    }

    template<typename LaneNode>
    auto get_lane_outputs(LaneNode const& node)
    {
        if constexpr (lane_node_details::has_static_outputs<LaneNode>) {
            return LaneNode::outputs();
        } else if constexpr (lane_node_details::has_member_outputs<LaneNode>) {
            return node.outputs();
        } else {
            return std::span<LaneOutputConfig const, 0> {};
        }
    }

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
