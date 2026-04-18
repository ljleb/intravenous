#pragma once

#ifdef IV_INTERNAL_TRANSLATION_UNIT
#error "dsl.h is reserved for user-authored DSL code; include graph/builder.h or module/module.h from internal code."
#endif

#include "module/module.h"

namespace iv {
    template<class Ref>
    concept SourceSpanAnnotatableRef = requires(std::remove_cvref_t<Ref> const& ref) {
        ref._add_source_span(0u, 0u);
    };

    template<class Ref>
    requires SourceSpanAnnotatableRef<Ref>
    std::remove_cvref_t<Ref> _add_node_source_span(Ref&& ref, uint32_t begin, uint32_t end)
    {
        using StoredRef = std::remove_cvref_t<Ref>;
        StoredRef stored_ref = std::forward<Ref>(ref);
        stored_ref._add_source_span(begin, end);
        return stored_ref;
    }

    template<class Ref>
    requires SourceSpanAnnotatableRef<Ref>
    std::remove_cvref_t<Ref> _add_node_source_span(
        Ref&& ref,
        std::string_view file_path,
        uint32_t begin,
        uint32_t end
    )
    {
        using StoredRef = std::remove_cvref_t<Ref>;
        StoredRef stored_ref = std::forward<Ref>(ref);
        if constexpr (requires {
            stored_ref._add_source_span(file_path, begin, end);
        }) {
            stored_ref._add_source_span(file_path, begin, end);
        } else {
            stored_ref._add_source_span(begin, end);
        }
        return stored_ref;
    }

    template<fixed_string Name>
    inline constexpr PortName<Name, NamedPortKind::sample> named{};

    template<fixed_string Name>
    consteval auto operator""_P()
    {
        return PortName<Name, NamedPortKind::sample>{};
    }

    template<fixed_string Name>
    consteval auto operator""_F()
    {
        return PortName<Name, NamedPortKind::event>{};
    }

    inline SamplePortRef lift(GraphBuilder& g, Sample value)
    {
        return g.node<Constant>(value);
    }

    inline SamplePortRef lift(SamplePortRef s)
    {
        if (!s.graph_builder) {
            details::error("cannot lift an empty sample port");
        }
        return s;
    }

    template<class T>
    concept SamplePortLike = std::convertible_to<std::remove_cvref_t<T>, SamplePortRef>;

    template<class T>
    concept EventPortLike = std::convertible_to<std::remove_cvref_t<T>, EventPortRef>;

    template<class T>
    concept NodeLike = std::convertible_to<std::remove_cvref_t<T>, NodeRef>;

    template<class T>
    EventPortRef lift_event_operand(T&& x)
    {
        if constexpr (EventPortLike<T>) {
            return std::forward<T>(x);
        } else if constexpr (NodeLike<T>) {
            return static_cast<NodeRef>(std::forward<T>(x)).event_port();
        }
    }

    template<class T>
    SamplePortRef lift_sample_operand(GraphBuilder& g, T&& x)
    {
        if constexpr (SamplePortLike<T>) {
            SamplePortRef s = std::forward<T>(x);
            if (s.graph_builder != &g) {
                details::error("operand belongs to a different builder");
            }
            return s;
        } else {
            return g.node<Constant>(static_cast<Sample>(x))();
        }
    }

    template<class T>
    concept ScalarLike =
        std::integral<std::remove_cvref_t<T>> ||
        std::floating_point<std::remove_cvref_t<T>> ||
        std::is_same_v<std::remove_cvref_t<T>, Sample>;

    template<class Node, class L, class R>
    requires ((SamplePortLike<L> || ScalarLike<L>) && (SamplePortLike<R> || ScalarLike<R>))
    NodeRef make_binary_op(L&& lhs, R&& rhs, std::string_view op_name)
    {
        GraphBuilder* g = nullptr;

        if constexpr (SamplePortLike<L>) {
            SamplePortRef s = std::forward<L>(lhs);
            g = s.graph_builder;
        }

        if constexpr (SamplePortLike<R>) {
            SamplePortRef s = std::forward<R>(rhs);
            if (!g) {
                g = s.graph_builder;
            } else if (s.graph_builder != g) {
                details::error(std::string(op_name) + ": operands belong to different builders");
            }
        }

        if (!g) {
            details::error(std::string(op_name) + ": at least one operand must be a sample port");
        }

        SamplePortRef lhs_sample_port = lift_sample_operand(*g, std::forward<L>(lhs));
        SamplePortRef rhs_sample_port = lift_sample_operand(*g, std::forward<R>(rhs));

        return g->node<Node>()(lhs_sample_port, rhs_sample_port);
    }

    template<class L, class R>
    requires ((SamplePortLike<L> || ScalarLike<L>) && (SamplePortLike<R> || ScalarLike<R>))
    NodeRef operator+(L&& lhs, R&& rhs)
    {
        return make_binary_op<Sum>(
            std::forward<L>(lhs),
            std::forward<R>(rhs),
            "operator+"
        );
    }

    template<class L, class R>
    requires ((SamplePortLike<L> || ScalarLike<L>) && (SamplePortLike<R> || ScalarLike<R>))
    NodeRef operator-(L&& lhs, R&& rhs)
    {
        return make_binary_op<Subtract>(
            std::forward<L>(lhs),
            std::forward<R>(rhs),
            "operator-"
        );
    }

    template<class L, class R>
    requires ((SamplePortLike<L> || ScalarLike<L>) && (SamplePortLike<R> || ScalarLike<R>))
    NodeRef operator*(L&& lhs, R&& rhs)
    {
        return make_binary_op<Product>(
            std::forward<L>(lhs),
            std::forward<R>(rhs),
            "operator*"
        );
    }

    template<class L, class R>
    requires ((SamplePortLike<L> || ScalarLike<L>) && (SamplePortLike<R> || ScalarLike<R>))
    NodeRef operator/(L&& lhs, R&& rhs)
    {
        return make_binary_op<Quotient>(
            std::forward<L>(lhs),
            std::forward<R>(rhs),
            "operator/"
        );
    }

    inline SamplePortRef operator~(SamplePortRef sample_port)
    {
        return sample_port.detach();
    }

    template<class T>
    requires (
        std::convertible_to<std::remove_cvref_t<T>, SamplePortRef> &&
        !std::same_as<std::remove_cvref_t<T>, SamplePortRef>
    )
    SamplePortRef operator~(T&& value)
    {
        return static_cast<SamplePortRef>(std::forward<T>(value)).detach();
    }

    template<class L, class R>
    requires (
        SamplePortLike<L> &&
        NodeLike<R>
    )
    auto connect_unary_node(L&& lhs, R&& rhs, std::string_view op_name)
    {
        SamplePortRef source = std::forward<L>(lhs);
        NodeRef target = static_cast<NodeRef>(std::forward<R>(rhs));

        auto const inputs = get_inputs(target.node());
        auto const outputs = get_outputs(target.node());

        if (inputs.size() != 1) {
            details::error(
                std::string(op_name) + " requires target to have exactly 1 input; got " +
                std::to_string(inputs.size()) + " inputs on " + target.to_string()
            );
        }

        target(source);
        if (outputs.empty()) {
            if constexpr (NodeLike<L>) {
                return target;
            } else {
                return source;
            }
        }
        if (outputs.size() != 1) {
            details::error(
                std::string(op_name) + " requires target to have at most 1 output when used as an expression; got " +
                std::to_string(outputs.size()) + " outputs on " + target.to_string()
            );
        }
        if constexpr (NodeLike<L>) {
            return target;
        } else {
            return static_cast<SamplePortRef>(target);
        }
    }

    template<class L, class R>
    requires (
        SamplePortLike<L> &&
        NodeLike<R>
    )
    auto operator>(L&& lhs, R&& rhs)
    {
        return connect_unary_node(std::forward<L>(lhs), std::forward<R>(rhs), "operator>");
    }

    template<class L, class R>
    requires (
        NodeLike<L> &&
        SamplePortLike<R>
    )
    auto operator<(L&& lhs, R&& rhs)
    {
        return connect_unary_node(std::forward<R>(rhs), std::forward<L>(lhs), "operator<");
    }

    template<class L, class R>
    requires (
        EventPortLike<L> &&
        NodeLike<R>
    )
    auto connect_unary_event_node(L&& lhs, R&& rhs, std::string_view op_name)
    {
        EventPortRef source = std::forward<L>(lhs);
        NodeRef target = static_cast<NodeRef>(std::forward<R>(rhs));

        auto const event_inputs = get_event_inputs(target.node());
        auto const event_outputs = get_event_outputs(target.node());

        if (event_inputs.size() != 1) {
            details::error(
                std::string(op_name) + " requires target to have exactly 1 event input; got " +
                std::to_string(event_inputs.size()) + " event inputs on " + target.to_string()
            );
        }

        target.connect_event_input(0, source);
        if (event_outputs.empty()) {
            return source;
        }
        if (event_outputs.size() != 1) {
            details::error(
                std::string(op_name) + " requires target to have at most 1 event output when used as an expression; got " +
                std::to_string(event_outputs.size()) + " event outputs on " + target.to_string()
            );
        }
        return target.event_port(0);
    }

    template<class L, class R>
    requires (
        EventPortLike<L> &&
        NodeLike<R>
    )
    auto operator>(L&& lhs, R&& rhs)
    {
        return connect_unary_event_node(std::forward<L>(lhs), std::forward<R>(rhs), "operator>");
    }

    template<class L, class R>
    requires (
        NodeLike<L> &&
        EventPortLike<R>
    )
    auto operator<(L&& lhs, R&& rhs)
    {
        return connect_unary_event_node(
            std::forward<R>(rhs),
            static_cast<NodeRef>(std::forward<L>(lhs)),
            "operator<"
        );
    }

    template<fixed_string Name, class R>
    requires NodeLike<R>
    SamplePortRef operator<<(PortName<Name, NamedPortKind::sample>, R&& rhs)
    {
        NodeRef node = static_cast<NodeRef>(std::forward<R>(rhs));
        return node[Name.view()];
    }

    template<class L, fixed_string Name>
    requires NodeLike<L>
    SamplePortRef operator>>(L&& lhs, PortName<Name, NamedPortKind::sample>)
    {
        NodeRef node = static_cast<NodeRef>(std::forward<L>(lhs));
        return node[Name.view()];
    }

    template<fixed_string Name, class R>
    requires NodeLike<R>
    EventPortRef operator<<(PortName<Name, NamedPortKind::event>, R&& rhs)
    {
        return static_cast<NodeRef>(std::forward<R>(rhs)).event_port(Name.view());
    }

    template<class L, fixed_string Name>
    requires NodeLike<L>
    EventPortRef operator>>(L&& lhs, PortName<Name, NamedPortKind::event>)
    {
        return static_cast<NodeRef>(std::forward<L>(lhs)).event_port(Name.view());
    }

    template<class Derived, class Node>
    template<class... Args>
    requires(details::node_call_enabled<Node, Args...>)
    Derived NodeRefBase<Derived, Node>::operator()(Args&&... args) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }

        auto const inputs = get_inputs(node());
        auto const event_inputs = get_event_inputs(node());

        auto connect_input = [&](SamplePortRef ref, size_t input_port, std::string_view input_name = {}) {
            if (input_port >= inputs.size()) {
                details::error(
                    to_string() + " "
                    "has at most " + std::to_string(inputs.size()) + " inputs, "
                    "got " + std::to_string(input_port + 1)
                );
            }

            if (ref.graph_builder != _graph_builder) {
                details::error(
                    ref.to_string() + " "
                    "does not belong to the same builder as " + to_string()
                );
            }

            PortId const input_port_id = { _index, input_port };
            if (_graph_builder->_placed_input_ports.contains(input_port_id)) {
                details::error(
                    "input port " + std::string(input_name.empty() ? inputs[input_port].name : input_name) + " of " + to_string() + " "
                    "was already placed in the graph"
                );
            }

            _graph_builder->_placed_input_ports.insert(input_port_id);
            _graph_builder->_edges.emplace(GraphEdge{
                ref,
                PortId{ _index, input_port },
            });
        };

        auto connect_event = [&](EventPortRef ref, size_t input_port, std::string_view input_name = {}) {
            if (input_port >= event_inputs.size()) {
                details::error(
                    to_string() + " "
                    "has at most " + std::to_string(event_inputs.size()) + " event inputs, "
                    "got " + std::to_string(input_port + 1)
                );
            }
            if (ref.graph_builder != _graph_builder) {
                details::error(
                    ref.to_string() + " "
                    "does not belong to the same builder as " + to_string()
                );
            }

            PortId const input_port_id = { _index, input_port };
            if (_graph_builder->_placed_event_input_ports.contains(input_port_id)) {
                details::error(
                    "event input port " + std::string(input_name.empty() ? event_inputs[input_port].name : input_name) + " of " + to_string() + " "
                    "was already placed in the graph"
                );
            }

            auto const source_type = (ref.node_index == GRAPH_ID)
                ? _graph_builder->_public_event_inputs[ref.output_port].type
                : _graph_builder->_nodes[ref.node_index].event_output_configs[ref.output_port].type;

            _graph_builder->_placed_event_input_ports.insert(input_port_id);
            _graph_builder->_event_edges.emplace(GraphEventEdge{
                PortId{ ref.node_index, ref.output_port },
                PortId{ _index, input_port },
                EventConversionRegistry::instance().plan(source_type, event_inputs[input_port].type)
            });
        };

        auto connect_named = [&](auto&& named_arg) {
            using NamedArgT = std::remove_cvref_t<decltype(named_arg)>;
            constexpr std::string_view name = NamedArgT::name.view();

            if constexpr (NamedArgT::kind == NamedPortKind::event) {
                for (size_t input_port = 0; input_port < event_inputs.size(); ++input_port) {
                    if (event_inputs[input_port].name == name) {
                        connect_event(lift_event_operand(named_arg.value), input_port, name);
                        return;
                    }
                }
            } else {
                for (size_t input_port = 0; input_port < inputs.size(); ++input_port) {
                    if (inputs[input_port].name == name) {
                        connect_input(_graph_builder->lift_to_sample_port(named_arg.value), input_port, name);
                        return;
                    }
                }
            }

            details::error(
                "an input port named '" + std::string(name) + "' "
                "does not exist on " + to_string()
            );
        };

        size_t positional_sample_port = 0;
        size_t positional_event_port = 0;
        auto const process_arg = [&](auto&& arg) {
            if constexpr (details::is_named_arg_v<decltype(arg)>) {
                connect_named(std::forward<decltype(arg)>(arg));
            } else if constexpr (EventPortLike<decltype(arg)>) {
                connect_event(static_cast<EventPortRef>(std::forward<decltype(arg)>(arg)), positional_event_port++);
            } else {
                connect_input(
                    _graph_builder->lift_to_sample_port(std::forward<decltype(arg)>(arg)),
                    positional_sample_port++
                );
            }
        };

        (process_arg(std::forward<Args>(args)), ...);

        return derived();
    }

    template<class Derived, class Node>
    template<class T>
    inline Derived NodeRefBase<Derived, Node>::connect_input(size_t input_port, T&& value) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }

        auto const inputs = get_inputs(node());
        if (input_port >= inputs.size()) {
            details::error(
                to_string() + " "
                "has at most " + std::to_string(inputs.size()) + " inputs, "
                "got " + std::to_string(input_port + 1)
            );
        }

        SamplePortRef ref = _graph_builder->lift_to_sample_port(std::forward<T>(value));
        if (ref.graph_builder != _graph_builder) {
            details::error(
                ref.to_string() + " "
                "does not belong to the same builder as " + to_string()
            );
        }

        PortId const input_port_id{ _index, input_port };
        if (_graph_builder->_placed_input_ports.contains(input_port_id)) {
            details::error(
                "input port " + inputs[input_port].name + " of " + to_string() + " "
                "was already placed in the graph"
            );
        }

        _graph_builder->_placed_input_ports.insert(input_port_id);
        _graph_builder->_edges.emplace(GraphEdge{
            ref,
            PortId{ _index, input_port },
        });
        return derived();
    }

    template<class Derived, class Node>
    template<class T>
    inline Derived NodeRefBase<Derived, Node>::connect_input(std::string_view input_name, T&& value) const
    {
        auto const inputs = get_inputs(node());
        std::optional<size_t> matched_input;
        for (size_t input_port = 0; input_port < inputs.size(); ++input_port) {
            if (inputs[input_port].name == input_name) {
                if (matched_input.has_value()) {
                    details::error(
                        "input name '" + std::string(input_name) + "' is ambiguous on " + to_string()
                    );
                }
                matched_input = input_port;
            }
        }

        if (matched_input.has_value()) {
            return connect_input(*matched_input, std::forward<T>(value));
        }

        details::error(
            "an input port named '" + std::string(input_name) + "' "
            "does not exist on " + to_string()
        );
    }

    template<class Derived, class Node>
    inline Derived NodeRefBase<Derived, Node>::connect_event_input(size_t input_port, EventPortRef value) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }

        auto const event_inputs = get_event_inputs(node());
        if (input_port >= event_inputs.size()) {
            details::error(
                to_string() + " "
                "has at most " + std::to_string(event_inputs.size()) + " event inputs, "
                "got " + std::to_string(input_port + 1)
            );
        }
        if (value.graph_builder != _graph_builder) {
            details::error(
                value.to_string() + " "
                "does not belong to the same builder as " + to_string()
            );
        }

        PortId const input_port_id{ _index, input_port };
        if (_graph_builder->_placed_event_input_ports.contains(input_port_id)) {
            details::error(
                "event input port " + event_inputs[input_port].name + " of " + to_string() + " "
                "was already placed in the graph"
            );
        }

        auto const source_type = (value.node_index == GRAPH_ID)
            ? _graph_builder->_public_event_inputs[value.output_port].type
            : _graph_builder->_nodes[value.node_index].event_output_configs[value.output_port].type;

        _graph_builder->_placed_event_input_ports.insert(input_port_id);
        _graph_builder->_event_edges.emplace(GraphEventEdge{
            PortId{ value.node_index, value.output_port },
            PortId{ _index, input_port },
            EventConversionRegistry::instance().plan(source_type, event_inputs[input_port].type)
        });
        return derived();
    }

    template<class Derived, class Node>
    inline Derived NodeRefBase<Derived, Node>::connect_event_input(std::string_view input_name, EventPortRef value) const
    {
        auto const event_inputs = get_event_inputs(node());
        std::optional<size_t> matched_input;
        for (size_t input_port = 0; input_port < event_inputs.size(); ++input_port) {
            if (event_inputs[input_port].name == input_name) {
                if (matched_input.has_value()) {
                    details::error(
                        "event input name '" + std::string(input_name) + "' is ambiguous on " + to_string()
                    );
                }
                matched_input = input_port;
            }
        }

        if (matched_input.has_value()) {
            return connect_event_input(*matched_input, value);
        }

        details::error(
            "an event input port named '" + std::string(input_name) + "' "
            "does not exist on " + to_string()
        );
    }

    template<size_t I, class Node>
    SamplePortRef get(StructuredNodeRef<Node> const& node_ref)
    {
        return node_ref.template get<I>();
    }

    template<size_t I, class Node>
    SamplePortRef get(StructuredNodeRef<Node>& node_ref)
    {
        return node_ref.template get<I>();
    }

    template<size_t I, class Node>
    SamplePortRef get(StructuredNodeRef<Node>&& node_ref)
    {
        return node_ref.template get<I>();
    }

    template<size_t voice_count, class Fn>
    NodeRef polyphonic(GraphBuilder& g, Fn&& make_voice)
    {
        static_assert(voice_count > 0, "iv::polyphonic requires at least one voice");

        std::vector<OutputConfig> output_configs;
        std::vector<EventOutputConfig> event_output_configs;
        NodeRef mix;

        auto const same_output_config = [](OutputConfig const& a, OutputConfig const& b) {
            return a.name == b.name
                && a.latency == b.latency
                && a.history == b.history;
        };
        auto const same_event_output_config = [](EventOutputConfig const& a, EventOutputConfig const& b) {
            return a.name == b.name
                && a.type == b.type;
        };

        auto connect_lane_outputs = [&](auto voice_index_c, NodeRef voice) {
            constexpr size_t voice_index = decltype(voice_index_c)::value;

            for (size_t output_port = 0; output_port < output_configs.size(); ++output_port) {
                mix.connect_input(output_port * voice_count + voice_index, voice[output_port]);
            }

            for (size_t output_port = 0; output_port < event_output_configs.size(); ++output_port) {
                mix.connect_event_input(
                    output_port * voice_count + voice_index,
                    voice.node_ref().event_port(output_port)
                );
            }
        };

        auto process_lane = [&](auto voice_index_c) {
            constexpr size_t voice_index = decltype(voice_index_c)::value;

            auto const midi_driver = g.node<MidiVoiceAllocator<voice_index, voice_count>>();
            NodeRef voice = make_voice(midi_driver).node_ref();
            auto const& voice_node = voice.node();
            if (
                voice_node.materialize == nullptr
                && voice_node.lowered_subgraph_count == 0
                && voice_node.subgraph_output_sources.empty()
                && voice_node.subgraph_event_output_sources.empty()
            ) {
                details::error("iv::polyphonic callback must return a materialized node or subgraph node");
            }

            if constexpr (voice_index == 0) {
                output_configs = voice_node.output_configs;
                event_output_configs = voice_node.event_output_configs;
                mix = g.node<PolyphonicMix>(
                    voice_count,
                    output_configs,
                    event_output_configs
                ).node_ref();
            } else {
                if (voice_node.output_configs.size() != output_configs.size()) {
                    details::error("iv::polyphonic voice lanes must expose the same number of sample outputs");
                }
                for (size_t i = 0; i < output_configs.size(); ++i) {
                    if (!same_output_config(voice_node.output_configs[i], output_configs[i])) {
                        details::error("iv::polyphonic voice lanes must expose matching sample output configs");
                    }
                }

                if (voice_node.event_output_configs.size() != event_output_configs.size()) {
                    details::error("iv::polyphonic voice lanes must expose the same number of event outputs");
                }
                for (size_t i = 0; i < event_output_configs.size(); ++i) {
                    if (!same_event_output_config(voice_node.event_output_configs[i], event_output_configs[i])) {
                        details::error("iv::polyphonic voice lanes must expose matching event output configs");
                    }
                }
            }

            connect_lane_outputs(voice_index_c, voice);
        };

        [&]<size_t... VoiceIndices>(std::index_sequence<VoiceIndices...>) {
            (process_lane(std::integral_constant<size_t, VoiceIndices>{}), ...);
        }(std::make_index_sequence<voice_count>{});

        return mix;
    }
}

namespace std {
    template<class Node>
    struct tuple_size<iv::StructuredNodeRef<Node>> :
        std::integral_constant<size_t, iv::details::fixed_output_count_v<Node>> {};

    template<size_t I, class Node>
    struct tuple_element<I, iv::StructuredNodeRef<Node>> {
        using type = iv::SamplePortRef;
    };
}
