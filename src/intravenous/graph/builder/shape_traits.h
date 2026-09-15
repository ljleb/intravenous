#pragma once

#include <intravenous/graph/builder/syntax.h>
#include <intravenous/node/traits.h>

#include <array>
#include <concepts>
#include <span>
#include <type_traits>

namespace iv::details {
    template<typename Inputs>
    struct fixed_input_count : std::integral_constant<size_t, std::dynamic_extent> {};

    template<size_t N>
    struct fixed_input_count<std::array<InputConfig, N>> : std::integral_constant<size_t, N> {};

    template<size_t N>
    struct fixed_input_count<std::span<InputConfig const, N>> : std::integral_constant<size_t, N> {};

    template<size_t N>
    struct fixed_input_count<std::span<InputConfig, N>> : std::integral_constant<size_t, N> {};

    template<typename Node>
    consteval size_t fixed_sample_input_count()
    {
        if constexpr (!has_inputs<Node>) {
            return 0;
        } else if constexpr (!has_constexpr_port_configs<Node>) {
            return std::dynamic_extent;
        } else {
            using Inputs = std::remove_cvref_t<decltype(Node::inputs())>;
            if constexpr (fixed_input_count<Inputs>::value == std::dynamic_extent)
                return std::dynamic_extent;
            else
                return count_sample_ports(Node::inputs());
        }
    }

    template<typename Node>
    inline constexpr size_t fixed_input_count_v = fixed_sample_input_count<Node>();

    template<>
    inline constexpr size_t fixed_input_count_v<void> = std::dynamic_extent;

    template<typename Node>
    inline constexpr bool has_fixed_input_count_v =
        (fixed_input_count_v<Node> != std::dynamic_extent);

    template<typename Node>
    consteval size_t fixed_event_input_count()
    {
        if constexpr (!has_inputs<Node>) {
            return 0;
        } else if constexpr (!has_constexpr_port_configs<Node>) {
            return std::dynamic_extent;
        } else {
            using Inputs = std::remove_cvref_t<decltype(Node::inputs())>;
            if constexpr (fixed_input_count<Inputs>::value == std::dynamic_extent)
                return std::dynamic_extent;
            else
                return count_event_ports(Node::inputs());
        }
    }

    template<typename Node>
    inline constexpr size_t fixed_event_input_count_v = fixed_event_input_count<Node>();

    template<>
    inline constexpr size_t fixed_event_input_count_v<void> = std::dynamic_extent;

    template<typename Node>
    inline constexpr bool has_fixed_event_input_count_v =
        (fixed_event_input_count_v<Node> != std::dynamic_extent);

    template<typename Outputs>
    struct fixed_output_count : std::integral_constant<size_t, std::dynamic_extent> {};

    template<size_t N>
    struct fixed_output_count<std::array<OutputConfig, N>> : std::integral_constant<size_t, N> {};

    template<size_t N>
    struct fixed_output_count<std::span<OutputConfig const, N>> : std::integral_constant<size_t, N> {};

    template<size_t N>
    struct fixed_output_count<std::span<OutputConfig, N>> : std::integral_constant<size_t, N> {};

    template<typename Node>
    consteval size_t fixed_sample_output_count()
    {
        if constexpr (!has_outputs<Node>) {
            return 0;
        } else if constexpr (!has_constexpr_port_configs<Node>) {
            return std::dynamic_extent;
        } else {
            using Outputs = std::remove_cvref_t<decltype(Node::outputs())>;
            if constexpr (fixed_output_count<Outputs>::value == std::dynamic_extent)
                return std::dynamic_extent;
            else
                return count_sample_ports(Node::outputs());
        }
    }

    template<typename Node>
    inline constexpr size_t fixed_output_count_v = fixed_sample_output_count<Node>();

    template<>
    inline constexpr size_t fixed_output_count_v<void> = std::dynamic_extent;

    template<typename Node>
    inline constexpr bool has_fixed_output_count_v =
        (fixed_output_count_v<Node> != std::dynamic_extent);

    template<typename Node>
    consteval size_t fixed_event_output_count()
    {
        if constexpr (!has_outputs<Node>) {
            return 0;
        } else if constexpr (!has_constexpr_port_configs<Node>) {
            return std::dynamic_extent;
        } else {
            using Outputs = std::remove_cvref_t<decltype(Node::outputs())>;
            if constexpr (fixed_output_count<Outputs>::value == std::dynamic_extent)
                return std::dynamic_extent;
            else
                return count_event_ports(Node::outputs());
        }
    }

    template<typename Node>
    inline constexpr size_t fixed_event_output_count_v = fixed_event_output_count<Node>();

    template<>
    inline constexpr size_t fixed_event_output_count_v<void> = std::dynamic_extent;

    template<typename Node>
    inline constexpr bool has_fixed_event_output_count_v =
        (fixed_event_output_count_v<Node> != std::dynamic_extent);

    template<typename Node>
    inline constexpr bool should_preserve_node_type_v =
        has_fixed_input_count_v<std::remove_cvref_t<Node>>
        || has_fixed_output_count_v<std::remove_cvref_t<Node>>
        || has_fixed_event_input_count_v<std::remove_cvref_t<Node>>
        || has_fixed_event_output_count_v<std::remove_cvref_t<Node>>;
}
