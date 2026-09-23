#pragma once

#include <intravenous/ports.h>

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
    struct TockCoverageContext;

    template<typename Node>
    struct PropagateForwardCoverageContext;

    template<typename Node>
    struct PropagateReverseCoverageContext;

    template<typename Node>
    struct TickSampleContext;

    template<typename Node>
    struct TickBlockContext;

    template<typename Node>
    struct SkipBlockContext;

    template<typename Node>
    void do_tock_coverage(Node const&, TockCoverageContext<Node>&);

    template<typename Node>
    void do_propagate_forward_coverage(
        Node const&, PropagateForwardCoverageContext<Node>&);

    template<typename Node>
    void do_propagate_reverse_coverage(
        Node const&, PropagateReverseCoverageContext<Node>&);

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

    // IndexedState is optional non-semantic acceleration state visible only to
    // tock_coverage(). It is deliberately separate from sequential Node::State
    // and from authoritative indexed-output storage.
    template<typename Node>
    struct NodeIndexedState {
        using Type = void;
    };

    namespace details {
        template<typename Node>
        concept has_IndexedState = requires {
            typename Node::IndexedState;
        };
    }

    template<typename Node>
    requires(details::has_IndexedState<Node>)
    struct NodeIndexedState<Node> {
        using Type = typename Node::IndexedState;
    };

    namespace details {
        template<typename Node>
        inline constexpr bool indexed_state_type_is_valid_v = [] {
            using IndexedState = typename NodeIndexedState<Node>::Type;
            if constexpr (std::is_void_v<IndexedState>) {
                return true;
            } else {
                return std::is_object_v<IndexedState>
                    && !std::is_const_v<IndexedState>
                    && !std::is_volatile_v<IndexedState>
                    && std::is_default_constructible_v<IndexedState>
                    && std::is_destructible_v<IndexedState>;
            }
        }();
    }

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

        template<typename Node>
        consteval bool declares_random_access_inputs()
        {
            if constexpr (!has_inputs<Node> || !has_constexpr_port_configs<Node>) {
                return false;
            } else {
                for (auto const& config : Node::inputs()) {
                    if (is_random_access(config)) return true;
                }
                return false;
            }
        }

        template<typename Node>
        consteval bool declares_tock_outputs()
        {
            if constexpr (!has_outputs<Node> || !has_constexpr_port_configs<Node>) {
                return false;
            } else {
                for (auto const& config : Node::outputs()) {
                    if (is_tock(config)) return true;
                }
                return false;
            }
        }

        template<typename Node>
        consteval bool declares_random_access_sample_inputs()
        {
            if constexpr (!has_inputs<Node> || !has_constexpr_port_configs<Node>) {
                return false;
            } else {
                for (auto const& config : Node::inputs()) {
                    if (is_sample(config) && is_random_access(config)) return true;
                }
                return false;
            }
        }

        template<typename Node>
        consteval bool declares_tock_sample_outputs()
        {
            if constexpr (!has_outputs<Node> || !has_constexpr_port_configs<Node>) {
                return false;
            } else {
                for (auto const& config : Node::outputs()) {
                    if (is_sample(config) && is_tock(config)) return true;
                }
                return false;
            }
        }

        template<typename Node>
        consteval bool declares_random_access_event_inputs()
        {
            if constexpr (!has_inputs<Node> || !has_constexpr_port_configs<Node>) {
                return false;
            } else {
                for (auto const& config : Node::inputs()) {
                    if (!is_sample(config) && is_random_access(config)) return true;
                }
                return false;
            }
        }

        template<typename Node>
        consteval bool declares_tock_event_outputs()
        {
            if constexpr (!has_outputs<Node> || !has_constexpr_port_configs<Node>) {
                return false;
            } else {
                for (auto const& config : Node::outputs()) {
                    if (!is_sample(config) && is_tock(config)) return true;
                }
                return false;
            }
        }

        template<typename Node>
        inline constexpr bool declares_random_access_inputs_v =
            declares_random_access_inputs<Node>();

        template<typename Node>
        inline constexpr bool declares_tock_outputs_v =
            declares_tock_outputs<Node>();

        template<typename Node>
        inline constexpr bool declares_random_access_sample_inputs_v =
            declares_random_access_sample_inputs<Node>();

        template<typename Node>
        inline constexpr bool declares_tock_sample_outputs_v =
            declares_tock_sample_outputs<Node>();

        template<typename Node>
        inline constexpr bool declares_random_access_event_inputs_v =
            declares_random_access_event_inputs<Node>();

        template<typename Node>
        inline constexpr bool declares_tock_event_outputs_v =
            declares_tock_event_outputs<Node>();

        template<typename Node>
        inline constexpr bool declares_random_access_or_tock_sample_ports_v =
            declares_random_access_sample_inputs_v<Node>
            || declares_tock_sample_outputs_v<Node>;

        template<typename Node>
        inline constexpr bool declares_random_access_or_tock_event_ports_v =
            declares_random_access_event_inputs_v<Node>
            || declares_tock_event_outputs_v<Node>;

        template<typename Node>
        concept has_tock_coverage = requires(Node const& node) {
            { node.tock_coverage(std::declval<TockCoverageContext<Node>&>()) }
                -> std::same_as<void>;
        };

        template<typename Node>
        concept has_propagate_forward_coverage = requires(Node const& node) {
            { node.propagate_forward_coverage(
                std::declval<PropagateForwardCoverageContext<Node>&>()) }
                -> std::same_as<void>;
        };

        template<typename Node>
        concept has_propagate_reverse_coverage = requires(Node const& node) {
            { node.propagate_reverse_coverage(
                std::declval<PropagateReverseCoverageContext<Node>&>()) }
                -> std::same_as<void>;
        };

        template<typename Node>
        concept has_tick = requires(
            Node const& node, TickSampleContext<Node> const& context) {
            { node.tick(context) } -> std::same_as<void>;
        };

        template<typename Node>
        concept has_tick_block = requires(
            Node const& node, TickBlockContext<Node> const& context) {
            { node.tick_block(context) } -> std::same_as<void>;
        };

        template<typename Node>
        concept has_skip_block = requires(
            Node const& node, SkipBlockContext<Node> const& context) {
            { node.skip_block(context) } -> std::same_as<void>;
        };

        template<typename Node>
        inline constexpr bool indexed_dsp_node_declaration_is_valid_v =
            indexed_state_type_is_valid_v<Node>
            && has_constexpr_port_configs<Node>
            && (!declares_tock_outputs_v<Node>
                || has_tock_coverage<Node>)
            && (!has_tock_coverage<Node>
                || declares_tock_outputs_v<Node>)
            && (!declares_tock_outputs_v<Node>
                || has_propagate_forward_coverage<Node>)
            && (!has_propagate_forward_coverage<Node>
                || declares_tock_outputs_v<Node>)
            && (!has_propagate_reverse_coverage<Node>
                || (declares_tock_outputs_v<Node>
                    && declares_random_access_inputs_v<Node>));

        // The author opts into a deterministic, side-effect-free Tick contract.
        // This is a type-level fact, not a producer mode; contextual replay is
        // established by the whole-graph planner after checking live sources.
        template<typename Node>
        consteval bool intrinsically_replayable()
        {
            if constexpr (requires { Node::intrinsically_replayable; }) {
                return std::bool_constant<Node::intrinsically_replayable>::value;
            } else {
                return false;
            }
        }

        template<typename Node>
        inline constexpr bool intrinsically_replayable_v =
            intrinsically_replayable<Node>();

        template<typename Node>
        consteval bool replay_ports_are_pointwise()
        {
            if constexpr (!has_constexpr_port_configs<Node>) {
                return false;
            } else {
                if constexpr (has_inputs<Node>) {
                    for (InputConfig const& config : Node::inputs()) {
                        if (!is_sequential(config)
                            || port_history_or_zero(config) != 0) return false;
                    }
                }
                if constexpr (has_outputs<Node>) {
                    for (OutputConfig const& config : Node::outputs()) {
                        if (!is_tick(config)
                            || port_history_or_zero(config) != 0
                            || tick_latency_or_zero(config) != 0) return false;
                    }
                }
                return true;
            }
        }

        template<typename Node>
        inline constexpr bool replay_declaration_is_valid_v =
            !intrinsically_replayable_v<Node>
            || (has_constexpr_port_configs<Node>
                && has_tick<Node>
                && !has_tick_block<Node>
                && !has_State<Node>
                && !has_IndexedState<Node>
                && replay_ports_are_pointwise<Node>());

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
        (void)node;
        static_assert(details::has_constexpr_port_configs<Node>,
            "concrete node ports must be declared by static constexpr inputs() and outputs()");
        if constexpr (details::has_outputs<Node>) return Node::outputs();
        else return std::span<OutputConfig const, 0>{};
    }

    template<typename Node>
    constexpr auto get_declared_inputs(Node const& node)
    {
        (void)node;
        static_assert(details::has_constexpr_port_configs<Node>,
            "concrete node ports must be declared by static constexpr inputs() and outputs()");
        if constexpr (details::has_inputs<Node>) return Node::inputs();
        else return std::span<InputConfig const, 0>{};
    }

    template<typename Node>
    std::vector<SampleInputConfig> get_inputs(Node const& node)
    {
        std::vector<SampleInputConfig> result;
        for (InputConfig const& input : get_declared_inputs(node)) {
            if (is_sample(input)) result.push_back(materialize_sample_config(input));
        }
        return result;
    }

    template<typename Node>
    std::vector<SampleOutputConfig> get_outputs(Node const& node)
    {
        std::vector<SampleOutputConfig> result;
        for (OutputConfig const& output : get_declared_outputs(node)) {
            if (is_sample(output)) result.push_back(materialize_sample_config(output));
        }
        return result;
    }

    template<typename Node>
    std::vector<EventInputConfig> get_event_inputs(Node const& node)
    {
        std::vector<EventInputConfig> result;
        for (InputConfig const& input : get_declared_inputs(node)) {
            if (!is_sample(input)) result.push_back(materialize_event_config(input));
        }
        return result;
    }

    template<typename Node>
    std::vector<EventOutputConfig> get_event_outputs(Node const& node)
    {
        std::vector<EventOutputConfig> result;
        for (OutputConfig const& output : get_declared_outputs(node)) {
            if (!is_sample(output)) result.push_back(materialize_event_config(output));
        }
        return result;
    }

    template<typename Node>
    constexpr size_t get_num_inputs(Node const& node)
    {
        return count_sample_ports(get_declared_inputs(node));
    }

    template<typename Node>
    constexpr size_t get_num_outputs(Node const& node)
    {
        return count_sample_ports(get_declared_outputs(node));
    }

    template<typename Node>
    constexpr size_t get_num_event_inputs(Node const& node)
    {
        return count_event_ports(get_declared_inputs(node));
    }

    template<typename Node>
    constexpr size_t get_num_event_outputs(Node const& node)
    {
        return count_event_ports(get_declared_outputs(node));
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
