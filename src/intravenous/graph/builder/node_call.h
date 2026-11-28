#pragma once

#include <intravenous/graph/builder/port_refs.h>
#include <intravenous/graph/builder/syntax.h>
#include <intravenous/graph/builder/shape_traits.h>

#include <concepts>
#include <optional>
#include <type_traits>
#include <utility>

namespace iv::details {
    template<class T>
    concept graph_builder_sample_port_like = std::convertible_to<std::remove_cvref_t<T>, SamplePortRef>;

    template<class T>
    concept graph_builder_event_port_like = std::convertible_to<std::remove_cvref_t<T>, EventPortRef>;

    // Keep this independent of dsl.h's NodeLike convenience concept.
    template<class T>
    concept graph_builder_node_output_like = requires(std::remove_cvref_t<T> const& value) {
        value.node_ref();
    };

    template<class T>
    consteval std::optional<PortKind> static_default_output_kind()
    {
        using Value = std::remove_cvref_t<T>;
        if constexpr (requires { typename Value::NodeType; }) {
            using Node = typename Value::NodeType;
            if constexpr (has_outputs<Node> && has_constexpr_port_configs<Node>) {
                if constexpr (port_config_count_v<decltype(Node::outputs())> != 0) {
                    return is_continuous(port_config_at<0>(Node::outputs()))
                        ? PortKind::sample : PortKind::event;
                }
            }
        }
        return std::nullopt;
    }

    template<class T>
    inline constexpr bool dynamic_node_output_kind_v =
        graph_builder_node_output_like<T> && !static_default_output_kind<T>();

    template<class T>
    constexpr EventPortRef lift_node_call_event_operand(T&& value)
    {
        if constexpr (graph_builder_event_port_like<T>) {
            return static_cast<EventPortRef>(std::forward<T>(value));
        } else {
            static_assert(graph_builder_node_output_like<T>,
                "an event input requires an event port or a node with an event output");
            return std::forward<T>(value).node_ref().event_port();
        }
    }

    template<class T>
    struct is_named_arg : std::false_type {};

    template<fixed_string Name, class T, NamedPortKind Kind>
    struct is_named_arg<NamedArg<Name, T, Kind>> : std::true_type {};

    template<class T>
    inline constexpr bool is_named_arg_v = is_named_arg<std::remove_cvref_t<T>>::value;

    template<class T>
    struct is_channel_named_arg : std::false_type {};

    template<fixed_string Name, class ChannelType, size_t ChannelIndex, class T>
    struct is_channel_named_arg<ChannelNamedArg<Name, ChannelType, ChannelIndex, T>> : std::true_type {};

    template<class T>
    inline constexpr bool is_channel_named_arg_v = is_channel_named_arg<std::remove_cvref_t<T>>::value;

    template<class T>
    struct is_default_channel_named_arg : std::false_type {};

    template<class ChannelType, size_t ChannelIndex, class T>
    struct is_default_channel_named_arg<DefaultChannelNamedArg<ChannelType, ChannelIndex, T>> : std::true_type {};

    template<class T>
    inline constexpr bool is_default_channel_named_arg_v = is_default_channel_named_arg<std::remove_cvref_t<T>>::value;

    template<class... Args>
    consteval bool named_args_follow_positionals_only()
    {
        if constexpr (sizeof...(Args) == 0) {
            return true;
        } else {
            bool seen_named = false;
            bool valid = true;
            ((valid = valid && (!seen_named || is_named_arg_v<Args>),
              seen_named = seen_named || is_named_arg_v<Args>), ...);
            return valid;
        }
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
    inline constexpr bool arg_targets_sample_input_v = [] {
        if constexpr (graph_builder_node_output_like<T>)
            return static_default_output_kind<T>() == PortKind::sample;
        else return !graph_builder_event_port_like<T>;
    }();

    template<fixed_string Name, class T, NamedPortKind Kind>
    inline constexpr bool arg_targets_sample_input_v<NamedArg<Name, T, Kind>> = (Kind == NamedPortKind::sample);

    template<class T>
    inline constexpr bool arg_targets_event_input_v = [] {
        if constexpr (graph_builder_node_output_like<T>)
            return static_default_output_kind<T>() == PortKind::event;
        else return graph_builder_event_port_like<T>;
    }();

    template<fixed_string Name, class T, NamedPortKind Kind>
    inline constexpr bool arg_targets_event_input_v<NamedArg<Name, T, Kind>> = (Kind == NamedPortKind::event);

    template<class... Args>
    inline constexpr size_t dynamic_input_arg_count_v =
        (0 + ... + (dynamic_node_output_kind_v<std::remove_cvref_t<Args>> ? 1u : 0u));

    template<class... Args>
    inline constexpr size_t sample_input_arg_count_v =
        (0 + ... + (arg_targets_sample_input_v<std::remove_cvref_t<Args>> ? 1u : 0u));

    template<class... Args>
    inline constexpr size_t event_input_arg_count_v =
        (0 + ... + (arg_targets_event_input_v<std::remove_cvref_t<Args>> ? 1u : 0u));

    template<class... Args>
    inline constexpr bool valid_node_call_args_v =
        named_args_follow_positionals_only_v<Args...>
        && unique_named_args_v<Args...>
        // Channel qualification is a public graph-port feature. Keeping it
        // out of node calls makes the current mono node-port contract explicit.
        && (!is_channel_named_arg_v<Args> && ...)
        && (!is_default_channel_named_arg_v<Args> && ...);

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
