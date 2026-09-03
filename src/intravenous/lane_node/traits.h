#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/lane_node/ui_state.h>
#include <intravenous/ports.h>

#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace iv {
    enum class LanePortDomain : std::uint8_t {
        compiled,
        realtime,
    };

    struct CompiledLanePortConfig {};
    struct RealtimeLanePortConfig {};

    using LanePortDomainConfig = std::variant<
        CompiledLanePortConfig,
        RealtimeLanePortConfig
    >;

    struct LaneInputPortConfig {
        Sample default_value = 0.0f;
    };

    struct LaneOutputPortConfig {};

    using LanePortDirectionConfig = std::variant<
        LaneInputPortConfig,
        LaneOutputPortConfig
    >;

    struct SampleLanePortConfig {
        SampleStreamLayout sample_layout = SampleStreamLayout::planar;
    };

    struct EventLanePortConfig {
        EventTypeId event_type = EventTypeId::empty;
    };

    using LanePortTypeConfig = std::variant<
        SampleLanePortConfig,
        EventLanePortConfig
    >;

    struct LanePortConfig {
        std::string name {};
        LanePortDomainConfig domain { RealtimeLanePortConfig {} };
        LanePortDirectionConfig direction { LaneOutputPortConfig {} };
        LanePortTypeConfig type { SampleLanePortConfig {} };
    };

    inline LanePortDomain lane_port_domain(LanePortConfig const& port)
    {
        return std::holds_alternative<CompiledLanePortConfig>(port.domain)
            ? LanePortDomain::compiled
            : LanePortDomain::realtime;
    }

    inline bool lane_port_is_input(LanePortConfig const& port)
    {
        return std::holds_alternative<LaneInputPortConfig>(port.direction);
    }

    inline bool lane_port_is_output(LanePortConfig const& port)
    {
        return std::holds_alternative<LaneOutputPortConfig>(port.direction);
    }

    inline PortKind lane_port_kind(LanePortConfig const& port)
    {
        return std::holds_alternative<EventLanePortConfig>(port.type)
            ? PortKind::event
            : PortKind::sample;
    }

    inline Sample lane_port_default_value(LanePortConfig const& port)
    {
        auto const* input = std::get_if<LaneInputPortConfig>(&port.direction);
        if (input == nullptr) {
            throw std::runtime_error("lane output port has no default input value");
        }
        return input->default_value;
    }

    inline SampleStreamLayout lane_port_sample_layout(LanePortConfig const& port)
    {
        auto const* sample = std::get_if<SampleLanePortConfig>(&port.type);
        if (sample == nullptr) {
            throw std::runtime_error("event lane port has no sample layout");
        }
        return sample->sample_layout;
    }

    inline EventTypeId lane_port_event_type(LanePortConfig const& port)
    {
        auto const* event = std::get_if<EventLanePortConfig>(&port.type);
        if (event == nullptr) {
            throw std::runtime_error("sample lane port has no event type");
        }
        return event->event_type;
    }

    inline std::optional<SampleStreamLayout> sample_stream_layout_for(
        LanePortConfig const& port)
    {
        auto const* sample = std::get_if<SampleLanePortConfig>(&port.type);
        if (sample == nullptr) {
            return std::nullopt;
        }
        return sample->sample_layout;
    }

    inline std::optional<ChannelLayout> sample_channel_layout_for(
        LanePortConfig const& port,
        std::optional<ChannelTypeId> sample_channel_type)
    {
        auto const sample_layout = sample_stream_layout_for(port);
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
        concept has_static_ports = requires {
            LaneNode::ports();
        };

        template<typename LaneNode>
        concept has_member_ports = requires(LaneNode const& node) {
            node.ports();
        };

        template<typename LaneNode>
        concept has_ports = has_static_ports<LaneNode> || has_member_ports<LaneNode>;

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
    decltype(auto) get_lane_ports(LaneNode const& node)
    {
        if constexpr (lane_node_details::has_static_ports<LaneNode>) {
            return LaneNode::ports();
        } else {
            static_assert(
                lane_node_details::has_member_ports<LaneNode>,
                "lane node must define ports()");
            return node.ports();
        }
    }

    template<typename LaneNode>
    LanePortConfig get_lane_output_port(LaneNode const& node)
    {
        LanePortConfig output;
        size_t output_count = 0;
        for (auto const& port : get_lane_ports(node)) {
            if (!lane_port_is_output(port)) {
                continue;
            }
            output = port;
            ++output_count;
        }
        if (output_count != 1) {
            throw std::runtime_error("lane node must declare exactly one output port");
        }
        return output;
    }

    template<typename LaneNode>
    LanePortDomain get_lane_output_domain(LaneNode const& node)
    {
        return lane_port_domain(get_lane_output_port(node));
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
