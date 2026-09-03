#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/lane_node/ui_state.h>
#include <intravenous/ports.h>

#include <array>
#include <concepts>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace iv {
    enum class LanePortDomain : std::uint8_t {
        compiled,
        realtime,
    };

    // Legacy declaration/runtime config types. These remain while existing
    // lane nodes migrate to the unified LanePortConfig declaration API.
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

    // Unified declaration-side config. Orthogonal concerns are represented by
    // independent variants instead of one variant over every combination.
    struct CompiledLanePortConfig {};
    struct RealtimeLanePortConfig {};
    using LanePortDomainConfig = std::variant<
        CompiledLanePortConfig,
        RealtimeLanePortConfig
    >;

    struct SampleLaneInputPortConfig {
        Sample default_value = 0.0f;
        SampleStreamLayout sample_layout = SampleStreamLayout::planar;
    };

    struct EventLaneInputPortConfig {
        EventTypeId event_type = EventTypeId::empty;
    };

    using LaneInputPortTypeConfig = std::variant<
        SampleLaneInputPortConfig,
        EventLaneInputPortConfig
    >;

    struct LaneInputPortConfig {
        LaneInputPortTypeConfig type {};
    };

    struct SampleLaneOutputPortConfig {
        SampleStreamLayout sample_layout = SampleStreamLayout::planar;
    };

    struct EventLaneOutputPortConfig {
        EventTypeId event_type = EventTypeId::empty;
    };

    using LaneOutputPortTypeConfig = std::variant<
        SampleLaneOutputPortConfig,
        EventLaneOutputPortConfig
    >;

    struct LaneOutputPortConfig {
        LaneOutputPortTypeConfig type {};
    };

    using LanePortDirectionConfig = std::variant<
        LaneInputPortConfig,
        LaneOutputPortConfig
    >;

    struct LanePortConfig {
        std::string name {};
        LanePortDomainConfig domain { RealtimeLanePortConfig {} };
        LanePortDirectionConfig direction { LaneOutputPortConfig {} };
    };

    template<typename Config>
    inline constexpr bool is_lane_input_config_v =
        std::same_as<std::remove_cvref_t<Config>, CompiledSampleLaneInputConfig>
        || std::same_as<std::remove_cvref_t<Config>, CompiledEventLaneInputConfig>
        || std::same_as<std::remove_cvref_t<Config>, RealtimeSampleLaneInputConfig>
        || std::same_as<std::remove_cvref_t<Config>, RealtimeEventLaneInputConfig>;

    template<typename Config>
    inline constexpr bool is_lane_output_config_v =
        std::same_as<std::remove_cvref_t<Config>, CompiledSampleLaneOutputConfig>
        || std::same_as<std::remove_cvref_t<Config>, CompiledEventLaneOutputConfig>
        || std::same_as<std::remove_cvref_t<Config>, RealtimeSampleLaneOutputConfig>
        || std::same_as<std::remove_cvref_t<Config>, RealtimeEventLaneOutputConfig>;

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
        concept has_static_ports = requires {
            LaneNode::ports();
        };

        template<typename LaneNode>
        concept has_member_ports = requires(LaneNode const& node) {
            node.ports();
        };

        template<typename LaneNode>
        concept has_ports = has_static_ports<LaneNode> || has_member_ports<LaneNode>;

        // Legacy declaration traits remain during the migration. New lane
        // nodes should declare their complete shape through ports().
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

        template<typename Fn>
        void visit_lane_port(LanePortConfig const& port, Fn&& fn)
        {
            bool const compiled = std::holds_alternative<CompiledLanePortConfig>(port.domain);
            std::visit([&](auto const& direction) {
                using Direction = std::remove_cvref_t<decltype(direction)>;
                if constexpr (std::same_as<Direction, LaneInputPortConfig>) {
                    std::visit([&](auto const& type) {
                        using Type = std::remove_cvref_t<decltype(type)>;
                        if constexpr (std::same_as<Type, SampleLaneInputPortConfig>) {
                            if (compiled) {
                                std::invoke(fn, CompiledSampleLaneInputConfig{
                                    .name = port.name,
                                    .default_value = type.default_value,
                                    .sample_layout = type.sample_layout,
                                });
                            } else {
                                std::invoke(fn, RealtimeSampleLaneInputConfig{
                                    .name = port.name,
                                    .default_value = type.default_value,
                                    .sample_layout = type.sample_layout,
                                });
                            }
                        } else {
                            if (compiled) {
                                std::invoke(fn, CompiledEventLaneInputConfig{
                                    .name = port.name,
                                    .event_type = type.event_type,
                                });
                            } else {
                                std::invoke(fn, RealtimeEventLaneInputConfig{
                                    .name = port.name,
                                    .event_type = type.event_type,
                                });
                            }
                        }
                    }, direction.type);
                } else {
                    std::visit([&](auto const& type) {
                        using Type = std::remove_cvref_t<decltype(type)>;
                        if constexpr (std::same_as<Type, SampleLaneOutputPortConfig>) {
                            if (compiled) {
                                std::invoke(fn, CompiledSampleLaneOutputConfig{
                                    .name = port.name,
                                    .sample_layout = type.sample_layout,
                                });
                            } else {
                                std::invoke(fn, RealtimeSampleLaneOutputConfig{
                                    .name = port.name,
                                    .sample_layout = type.sample_layout,
                                });
                            }
                        } else {
                            if (compiled) {
                                std::invoke(fn, CompiledEventLaneOutputConfig{
                                    .name = port.name,
                                    .event_type = type.event_type,
                                });
                            } else {
                                std::invoke(fn, RealtimeEventLaneOutputConfig{
                                    .name = port.name,
                                    .event_type = type.event_type,
                                });
                            }
                        }
                    }, direction.type);
                }
            }, port.direction);
        }

        template<typename Ports, typename Fn>
        void for_each_ports_value(Ports const& ports, Fn&& fn)
        {
            using PortsType = std::remove_cvref_t<Ports>;
            if constexpr (std::same_as<PortsType, LanePortConfig>) {
                visit_lane_port(ports, std::forward<Fn>(fn));
            } else if constexpr (requires { std::tuple_size<PortsType>::value; }) {
                std::apply([&](auto const&... port) {
                    static_assert((std::same_as<std::remove_cvref_t<decltype(port)>, LanePortConfig> && ...),
                        "ports() tuple elements must be LanePortConfig");
                    (visit_lane_port(port, fn), ...);
                }, ports);
            } else {
                for (auto const& port : ports) {
                    static_assert(
                        std::same_as<std::remove_cvref_t<decltype(port)>, LanePortConfig>,
                        "ports() range elements must be LanePortConfig");
                    visit_lane_port(port, fn);
                }
            }
        }

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
                "legacy lane node must define output()");
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

    template<typename LaneNode, typename Fn>
    void for_each_lane_port(LaneNode const& node, Fn&& fn)
    {
        if constexpr (lane_node_details::has_ports<LaneNode>) {
            auto const ports = get_lane_ports(node);
            lane_node_details::for_each_ports_value(ports, std::forward<Fn>(fn));
        } else {
            auto visit_range = [&](auto const& ports) {
                for (auto const& port : ports) {
                    std::invoke(fn, port);
                }
            };
            visit_range(get_compiled_sample_lane_inputs(node));
            visit_range(get_compiled_event_lane_inputs(node));
            visit_range(get_realtime_sample_lane_inputs(node));
            visit_range(get_realtime_event_lane_inputs(node));
            std::visit(std::forward<Fn>(fn), normalize_lane_output(get_lane_output(node)));
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
        LaneOutputConfig output {};
        size_t output_count = 0;
        for_each_lane_port(node, [&](auto const& port) {
            using Config = std::remove_cvref_t<decltype(port)>;
            if constexpr (is_lane_output_config_v<Config>) {
                output = LaneOutputConfig { port };
                ++output_count;
            }
        });
        if (output_count != 1) {
            throw std::runtime_error("lane node must declare exactly one output port");
        }
        return lane_output_domain(output);
    }

    namespace lane_node_details {
        template<typename LaneNode, bool = has_ports<LaneNode>>
        struct output_declaration_for_node {
            using type = decltype(get_lane_output(std::declval<LaneNode const&>()));
        };

        template<typename LaneNode>
        struct output_declaration_for_node<LaneNode, true> {
            using type = LaneOutputConfig;
        };
    }

    template<typename LaneNode>
    using LaneOutputDeclaration = typename lane_node_details::output_declaration_for_node<LaneNode>::type;

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
