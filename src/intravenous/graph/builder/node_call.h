#pragma once

#include <intravenous/graph/builder/port_refs.h>
#include <intravenous/graph/builder/syntax.h>
#include <intravenous/graph/builder/shape_traits.h>

#include <concepts>
#include <type_traits>

namespace iv::details {
    template<class T>
    concept graph_builder_sample_port_like = std::convertible_to<std::remove_cvref_t<T>, SamplePortRef>;

    template<class T>
    concept graph_builder_event_port_like = std::convertible_to<std::remove_cvref_t<T>, EventPortRef>;

    template<class T>
    struct is_named_arg : std::false_type {};

    template<fixed_string Name, class T, NamedPortKind Kind>
    struct is_named_arg<NamedArg<Name, T, Kind>> : std::true_type {};

    template<class T>
    inline constexpr bool is_named_arg_v = is_named_arg<std::remove_cvref_t<T>>::value;

    template<class... Args>
    consteval bool named_args_follow_positionals_only()
    {
        bool seen_named = false;
        bool valid = true;
        ((valid = valid && (!seen_named || is_named_arg_v<Args>),
          seen_named = seen_named || is_named_arg_v<Args>), ...);
        return valid;
    }

    template<class... Args>
    inline constexpr bool named_args_follow_positionals_only_v =
        named_args_follow_positionals_only<Args...>();

    template<class A, class B>
    inline constexpr bool same_named_port_v = false;

    template<fixed_string A, class TA, NamedPortKind KA, fixed_string B, class TB, NamedPortKind KB>
    inline constexpr bool same_named_port_v<NamedArg<A, TA, KA>, NamedArg<B, TB, KB>> =
        (A.view() == B.view()) && (KA == KB);

    template<class Arg, class... Rest>
    inline constexpr bool named_port_not_in_rest_v =
        (!same_named_port_v<std::remove_cvref_t<Arg>, std::remove_cvref_t<Rest>> && ...);

    template<class... Args>
    struct unique_named_args : std::true_type {};

    template<class Arg, class... Rest>
    struct unique_named_args<Arg, Rest...> : std::bool_constant<
        (!is_named_arg_v<Arg> || named_port_not_in_rest_v<Arg, Rest...>)
        && unique_named_args<Rest...>::value
    > {};

    template<class... Args>
    inline constexpr bool unique_named_args_v = unique_named_args<Args...>::value;

    template<class T>
    inline constexpr bool arg_targets_sample_input_v = !std::convertible_to<std::remove_cvref_t<T>, EventPortRef>;

    template<fixed_string Name, class T, NamedPortKind Kind>
    inline constexpr bool arg_targets_sample_input_v<NamedArg<Name, T, Kind>> = (Kind == NamedPortKind::sample);

    template<class T>
    inline constexpr bool arg_targets_event_input_v = std::convertible_to<std::remove_cvref_t<T>, EventPortRef>;

    template<fixed_string Name, class T, NamedPortKind Kind>
    inline constexpr bool arg_targets_event_input_v<NamedArg<Name, T, Kind>> = (Kind == NamedPortKind::event);

    template<class... Args>
    inline constexpr size_t sample_input_arg_count_v =
        (0 + ... + (arg_targets_sample_input_v<std::remove_cvref_t<Args>> ? 1u : 0u));

    template<class... Args>
    inline constexpr size_t event_input_arg_count_v =
        (0 + ... + (arg_targets_event_input_v<std::remove_cvref_t<Args>> ? 1u : 0u));

    template<class... Args>
    inline constexpr bool valid_node_call_args_v =
        named_args_follow_positionals_only_v<Args...>
        && unique_named_args_v<Args...>;

    template<class Node, class... Args>
    concept node_call_enabled =
        valid_node_call_args_v<Args...>
        && (
            std::same_as<Node, void>
            || !has_fixed_input_count_v<Node>
            || sample_input_arg_count_v<Args...> <= fixed_input_count_v<Node>
        )
        && (
            std::same_as<Node, void>
            || !has_fixed_event_input_count_v<Node>
            || event_input_arg_count_v<Args...> <= fixed_event_input_count_v<Node>
        );
}
