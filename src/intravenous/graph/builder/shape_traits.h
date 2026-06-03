#pragma once

#include <intravenous/graph/builder/syntax.h>
#include <intravenous/graph/node.h>

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
    inline constexpr size_t fixed_input_count_v = fixed_input_count<
        std::remove_cvref_t<decltype(get_inputs(std::declval<Node const&>()))>
    >::value;

    template<>
    inline constexpr size_t fixed_input_count_v<void> = std::dynamic_extent;

    template<typename Node>
    inline constexpr bool has_fixed_input_count_v =
        (fixed_input_count_v<Node> != std::dynamic_extent);

    template<typename EventInputs>
    struct fixed_event_input_count : std::integral_constant<size_t, std::dynamic_extent> {};

    template<size_t N>
    struct fixed_event_input_count<std::array<EventInputConfig, N>> : std::integral_constant<size_t, N> {};

    template<size_t N>
    struct fixed_event_input_count<std::span<EventInputConfig const, N>> : std::integral_constant<size_t, N> {};

    template<size_t N>
    struct fixed_event_input_count<std::span<EventInputConfig, N>> : std::integral_constant<size_t, N> {};

    template<typename Node>
    inline constexpr size_t fixed_event_input_count_v = fixed_event_input_count<
        std::remove_cvref_t<decltype(get_event_inputs(std::declval<Node const&>()))>
    >::value;

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
    inline constexpr size_t fixed_output_count_v = fixed_output_count<
        std::remove_cvref_t<decltype(get_outputs(std::declval<Node const&>()))>
    >::value;

    template<>
    inline constexpr size_t fixed_output_count_v<void> = std::dynamic_extent;

    template<typename Node>
    inline constexpr bool has_fixed_output_count_v =
        (fixed_output_count_v<Node> != std::dynamic_extent);

    template<typename EventOutputs>
    struct fixed_event_output_count : std::integral_constant<size_t, std::dynamic_extent> {};

    template<size_t N>
    struct fixed_event_output_count<std::array<EventOutputConfig, N>> : std::integral_constant<size_t, N> {};

    template<size_t N>
    struct fixed_event_output_count<std::span<EventOutputConfig const, N>> : std::integral_constant<size_t, N> {};

    template<size_t N>
    struct fixed_event_output_count<std::span<EventOutputConfig, N>> : std::integral_constant<size_t, N> {};

    template<typename Node>
    inline constexpr size_t fixed_event_output_count_v = fixed_event_output_count<
        std::remove_cvref_t<decltype(get_event_outputs(std::declval<Node const&>()))>
    >::value;

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
