#pragma once

#include <intravenous/ports.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace iv {
    template<typename Node>
    struct NodeState {
        using Type = void;
    };

    namespace details {
        template<typename Node>
        concept has_State = requires {
            typename Node::State;
        };
    }

    template<typename Node>
    requires(details::has_State<Node>)
    struct NodeState<Node> {
        using Type = typename Node::State;
    };

    template<typename A>
    struct NoCopy : public A
    {
        NoCopy(NoCopy const&) = delete;
        NoCopy(NoCopy&&) = delete;
    };

    template<>
    struct NoCopy<void>
    {
        NoCopy(NoCopy const&) = delete;
        NoCopy(NoCopy&&) = delete;
    };

    namespace details
    {
        template <typename Node>
        concept has_outputs = requires(Node const& node)
        {
            std::begin(node.outputs());
            std::end(node.outputs());
        };

        template <typename Node>
        concept has_num_outputs = requires(Node node, size_t num_outputs)
        {
            num_outputs = node.num_outputs();
        };

        template <typename Node>
        concept has_inputs = requires(Node const& node)
        {
            std::begin(node.inputs());
            std::end(node.inputs());
        };

        template<typename Range, typename Config>
        concept port_config_range =
            std::ranges::input_range<Range>
            && std::convertible_to<std::ranges::range_value_t<Range>, Config>;

        template<typename Node>
        concept has_declared_inputs =
            has_inputs<Node>
            && port_config_range<decltype(std::declval<Node const&>().inputs()), InputConfig>;

        template<typename Node>
        concept has_declared_outputs =
            has_outputs<Node>
            && port_config_range<decltype(std::declval<Node const&>().outputs()), OutputConfig>;

        template<typename Node>
        concept has_sample_inputs =
            has_inputs<Node>
            && port_config_range<decltype(std::declval<Node const&>().inputs()), SampleInputConfig>;

        template<typename Node>
        concept has_sample_outputs =
            has_outputs<Node>
            && port_config_range<decltype(std::declval<Node const&>().outputs()), SampleOutputConfig>;

        template<typename Node>
        concept has_event_inputs = requires(Node const& node)
        {
            std::begin(node.event_inputs());
            std::end(node.event_inputs());
        };

        template<typename Node>
        concept has_event_outputs = requires(Node const& node)
        {
            std::begin(node.event_outputs());
            std::end(node.event_outputs());
        };

        template<typename Node>
        concept has_static_port_config_members =
            (!has_inputs<Node> || requires {
                { Node::inputs() };
                requires port_config_range<decltype(Node::inputs()), InputConfig>;
            })
            && (!has_outputs<Node> || requires {
                { Node::outputs() };
                requires port_config_range<decltype(Node::outputs()), OutputConfig>;
            });

        template<typename Node>
        consteval bool constexpr_port_configs_available()
        {
            if constexpr (requires { Node::inputs(); }) {
                (void)Node::inputs();
            }
            if constexpr (requires { Node::outputs(); }) {
                (void)Node::outputs();
            }
            return true;
        }

        template<typename Node>
        concept has_constexpr_port_configs =
            has_static_port_config_members<Node>
            && requires {
                std::bool_constant<constexpr_port_configs_available<Node>()>{};
            };

        template<typename Configs>
        struct fixed_port_config_count
            : std::integral_constant<size_t, std::dynamic_extent> {};

        template<size_t N>
        struct fixed_port_config_count<std::array<InputConfig, N>>
            : std::integral_constant<size_t, N> {};

        template<size_t N>
        struct fixed_port_config_count<std::span<InputConfig const, N>>
            : std::integral_constant<size_t, N> {};

        template<size_t N>
        struct fixed_port_config_count<std::span<InputConfig, N>>
            : std::integral_constant<size_t, N> {};

        template<size_t N>
        struct fixed_port_config_count<std::array<OutputConfig, N>>
            : std::integral_constant<size_t, N> {};

        template<size_t N>
        struct fixed_port_config_count<std::span<OutputConfig const, N>>
            : std::integral_constant<size_t, N> {};

        template<size_t N>
        struct fixed_port_config_count<std::span<OutputConfig, N>>
            : std::integral_constant<size_t, N> {};

        template<typename Node, bool Sample>
        consteval size_t fixed_num_inputs()
        {
            if constexpr (!has_inputs<Node>) {
                return 0;
            } else if constexpr (!has_constexpr_port_configs<Node>) {
                return std::dynamic_extent;
            } else {
                using Inputs = std::remove_cvref_t<decltype(Node::inputs())>;
                if constexpr (
                    fixed_port_config_count<Inputs>::value == std::dynamic_extent) {
                    return std::dynamic_extent;
                } else {
                    return Sample
                        ? count_sample_ports(Node::inputs())
                        : count_event_ports(Node::inputs());
                }
            }
        }

        template<typename Node, bool Sample>
        consteval size_t fixed_num_outputs()
        {
            if constexpr (!has_outputs<Node>) {
                return 0;
            } else if constexpr (!has_constexpr_port_configs<Node>) {
                return std::dynamic_extent;
            } else {
                using Outputs = std::remove_cvref_t<decltype(Node::outputs())>;
                if constexpr (
                    fixed_port_config_count<Outputs>::value == std::dynamic_extent) {
                    return std::dynamic_extent;
                } else {
                    return Sample
                        ? count_sample_ports(Node::outputs())
                        : count_event_ports(Node::outputs());
                }
            }
        }

        template<typename Node>
        inline constexpr size_t fixed_num_inputs_v = fixed_num_inputs<Node, true>();

        template<typename Node>
        inline constexpr size_t fixed_num_event_inputs_v = fixed_num_inputs<Node, false>();

        template<typename Node>
        inline constexpr size_t fixed_num_outputs_v = fixed_num_outputs<Node, true>();

        template<typename Node>
        inline constexpr size_t fixed_num_event_outputs_v = fixed_num_outputs<Node, false>();

        template<>
        inline constexpr size_t fixed_num_inputs_v<void> = std::dynamic_extent;

        template<>
        inline constexpr size_t fixed_num_event_inputs_v<void> = std::dynamic_extent;

        template<>
        inline constexpr size_t fixed_num_outputs_v<void> = std::dynamic_extent;

        template<>
        inline constexpr size_t fixed_num_event_outputs_v<void> = std::dynamic_extent;

        template<typename Node>
        inline constexpr bool has_fixed_num_inputs_v =
            fixed_num_inputs_v<Node> != std::dynamic_extent;

        template<typename Node>
        inline constexpr bool has_fixed_num_event_inputs_v =
            fixed_num_event_inputs_v<Node> != std::dynamic_extent;

        template<typename Node>
        inline constexpr bool has_fixed_num_outputs_v =
            fixed_num_outputs_v<Node> != std::dynamic_extent;

        template<typename Node>
        inline constexpr bool has_fixed_num_event_outputs_v =
            fixed_num_event_outputs_v<Node> != std::dynamic_extent;

        template <typename Node>
        concept has_internal_latency = requires(Node node, size_t internal_latency)
        {
            internal_latency = node.internal_latency();
        };

        template <typename Node>
        concept has_max_block_size_method = requires(Node node, size_t block_size)
        {
            block_size = node.max_block_size();
        };

        template <typename Node>
        concept has_ttl_method = requires(Node node, std::optional<size_t> ttl)
        {
            ttl = node.ttl_samples();
        };

        template <typename Node>
        concept has_can_skip_block_method = requires(Node const& node, bool value)
        {
            value = node.can_skip_block();
        };

    }

    template<typename Node>
    constexpr auto get_declared_outputs(Node const& node)
    {
        if constexpr (details::has_declared_outputs<Node>)
        {
            return node.outputs();
        }
        else
        {
            return std::span<OutputConfig const, 0>{};
        }
    }

    template<typename Node>
    constexpr auto get_declared_inputs(Node const& node)
    {
        if constexpr (details::has_declared_inputs<Node>)
        {
            return node.inputs();
        }
        else
        {
            return std::span<InputConfig const, 0>{};
        }
    }

    template<typename Node>
    std::vector<SampleInputConfig> get_inputs(Node const& node)
    {
        if constexpr (details::has_sample_inputs<Node>) {
            auto const configs = node.inputs();
            return {configs.begin(), configs.end()};
        }
        std::vector<SampleInputConfig> result;
        for (InputConfig const& input : get_declared_inputs(node)) {
            if (is_sample(input)) result.push_back(materialize_sample_config(input));
        }
        return result;
    }

    template<typename Node>
    std::vector<SampleOutputConfig> get_outputs(Node const& node)
    {
        if constexpr (details::has_sample_outputs<Node>) {
            auto const configs = node.outputs();
            return {configs.begin(), configs.end()};
        }
        std::vector<SampleOutputConfig> result;
        for (OutputConfig const& output : get_declared_outputs(node)) {
            if (is_sample(output)) result.push_back(materialize_sample_config(output));
        }
        return result;
    }

    template<typename Node>
    std::vector<EventInputConfig> get_event_inputs(Node const& node)
    {
        if constexpr (details::has_event_inputs<Node>) {
            auto const configs = node.event_inputs();
            return {configs.begin(), configs.end()};
        }
        std::vector<EventInputConfig> result;
        for (InputConfig const& input : get_declared_inputs(node)) {
            if (!is_sample(input)) result.push_back(materialize_event_config(input));
        }
        return result;
    }

    template<typename Node>
    std::vector<EventOutputConfig> get_event_outputs(Node const& node)
    {
        if constexpr (details::has_event_outputs<Node>) {
            auto const configs = node.event_outputs();
            return {configs.begin(), configs.end()};
        }
        std::vector<EventOutputConfig> result;
        for (OutputConfig const& output : get_declared_outputs(node)) {
            if (!is_sample(output)) result.push_back(materialize_event_config(output));
        }
        return result;
    }

    template<typename Node>
    constexpr size_t get_num_inputs(Node const& node)
    {
        if constexpr (details::has_sample_inputs<Node>) {
            return node.inputs().size();
        } else {
            return count_sample_ports(get_declared_inputs(node));
        }
    }

    template<typename Node>
    constexpr size_t get_num_outputs(Node const& node)
    {
        if constexpr (details::has_sample_outputs<Node>) {
            return node.outputs().size();
        } else {
            return count_sample_ports(get_declared_outputs(node));
        }
    }

    template<typename Node>
    constexpr size_t get_num_event_inputs(Node const& node)
    {
        if constexpr (details::has_event_inputs<Node>) {
            return node.event_inputs().size();
        } else {
            return count_event_ports(get_declared_inputs(node));
        }
    }

    template<typename Node>
    constexpr size_t get_num_event_outputs(Node const& node)
    {
        if constexpr (details::has_event_outputs<Node>) {
            return node.event_outputs().size();
        } else {
            return count_event_ports(get_declared_outputs(node));
        }
    }

    template<typename Node>
    constexpr size_t get_internal_latency(Node const& node)
    {
        if constexpr (details::has_internal_latency<Node>)
        {
            return node.internal_latency();
        }
        else
        {
            return 0;
        }
    }

    template<typename Node>
    constexpr size_t get_max_block_size(Node const& node)
    {
        if constexpr (details::has_max_block_size_method<Node>)
        {
            return node.max_block_size();
        }
        else
        {
            return MAX_BLOCK_SIZE;
        }
    }

    template<typename Node>
    constexpr std::optional<size_t> get_ttl_samples(Node const& node)
    {
        if constexpr (details::has_ttl_method<Node>)
        {
            return node.ttl_samples();
        }
        else
        {
            return std::nullopt;
        }
    }

    template<typename Node>
    constexpr bool get_can_skip_block(Node const& node)
    {
        if constexpr (details::has_can_skip_block_method<Node>) {
            return node.can_skip_block();
        } else {
            return get_num_event_outputs(node) == 0
                && get_num_outputs(node) > 0
                && (get_num_inputs(node) > 0
                    || get_num_event_inputs(node) > 0);
        }
    }

}
