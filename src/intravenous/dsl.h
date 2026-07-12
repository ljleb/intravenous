#pragma once

#ifdef IV_INTERNAL_TRANSLATION_UNIT
#error "dsl.h is reserved for user-authored DSL code; include graph/builder.h or module/module.h from internal code."
#endif

#include <intravenous/basic_nodes/midi.h>
#include <intravenous/channel_ports.h>
#include <intravenous/module/module.h>

namespace iv {
    template<class Ref>
    concept SourceInfoAnnotatableRef =
        requires(std::remove_cvref_t<Ref>& ref, std::string_view declaration_identity) {
            ref.node_ref();
        } &&
        requires(std::remove_cvref_t<Ref>& ref, std::string_view declaration_identity) {
        ref._annotate_source_info(declaration_identity, std::string_view {}, 0u, 0u);
    };

    template<class Ref>
    requires SourceInfoAnnotatableRef<Ref>
    Ref&& _annotate_node_source_info(
        Ref&& ref,
        std::string_view declaration_identity
    )
    {
        ref._annotate_source_info(declaration_identity, std::string_view {}, 0u, 0u);
        return std::forward<Ref>(ref);
    }

    inline PublicSampleInputRef&& _annotate_public_input_source_info(
        PublicSampleInputRef&& ref,
        std::string_view declaration_identity,
        std::string_view file_path,
        uint32_t begin,
        uint32_t end)
    {
        ref._annotate_source_info(declaration_identity, file_path, begin, end);
        return std::move(ref);
    }

    inline PublicSampleInputRef& _annotate_public_input_source_info(
        PublicSampleInputRef& ref,
        std::string_view declaration_identity,
        std::string_view file_path,
        uint32_t begin,
        uint32_t end)
    {
        ref._annotate_source_info(declaration_identity, file_path, begin, end);
        return ref;
    }

    inline PublicSampleInputRef const& _annotate_public_input_source_info(
        PublicSampleInputRef const& ref,
        std::string_view declaration_identity,
        std::string_view file_path,
        uint32_t begin,
        uint32_t end)
    {
        ref._annotate_source_info(declaration_identity, file_path, begin, end);
        return ref;
    }

    template<class Ref>
    requires SourceInfoAnnotatableRef<Ref>
    Ref&& _annotate_node_source_info(
        Ref&& ref,
        std::string_view declaration_identity,
        std::string_view file_path,
        uint32_t begin,
        uint32_t end
    )
    {
        ref._annotate_source_info(declaration_identity, file_path, begin, end);
        return std::forward<Ref>(ref);
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
    requires NodeLike<T>
    NodeRef _materialize_node_ref(T&& value)
    {
        if constexpr (requires(std::remove_cvref_t<T> const& ref) { ref.node_ref(); }) {
            return value.node_ref();
        } else {
            return static_cast<NodeRef>(std::forward<T>(value));
        }
    }

    template<class T>
    EventPortRef lift_event_operand(T&& x)
    {
        if constexpr (EventPortLike<T>) {
            return std::forward<T>(x);
        } else if constexpr (NodeLike<T>) {
            return _materialize_node_ref(std::forward<T>(x)).event_port();
        }
    }

    template<class T>
    SamplePortRef lift_sample_operand(GraphBuilder& g, T&& x)
    {
        if constexpr (SamplePortLike<T>) {
            SamplePortRef s = static_cast<SamplePortRef>(std::forward<T>(x));
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
            SamplePortRef s = static_cast<SamplePortRef>(std::forward<L>(lhs));
            g = s.graph_builder;
        }

        if constexpr (SamplePortLike<R>) {
            SamplePortRef s = static_cast<SamplePortRef>(std::forward<R>(rhs));
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
        return make_binary_op<Sum<2>>(
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
        return make_binary_op<Product<2>>(
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

    inline SamplePortRef operator~(SamplePortRef const& sample_port)
    {
        return sample_port.detach();
    }

    inline SamplePortRef operator~(SamplePortRef&& sample_port)
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
        SamplePortRef source = static_cast<SamplePortRef>(std::forward<L>(lhs));
        NodeRef target = _materialize_node_ref(std::forward<R>(rhs));

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
        NodeRef target = _materialize_node_ref(std::forward<R>(rhs));

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
            _materialize_node_ref(std::forward<L>(lhs)),
            "operator<"
        );
    }

    template<fixed_string Name, class R>
    requires NodeLike<R>
    SamplePortRef operator<<(PortName<Name, NamedPortKind::sample>, R&& rhs)
    {
        NodeRef node = _materialize_node_ref(std::forward<R>(rhs));
        return node[Name.view()];
    }

    template<class L, fixed_string Name>
    requires NodeLike<L>
    SamplePortRef operator>>(L&& lhs, PortName<Name, NamedPortKind::sample>)
    {
        NodeRef node = _materialize_node_ref(std::forward<L>(lhs));
        return node[Name.view()];
    }

    template<fixed_string Name, class R>
    requires NodeLike<R>
    EventPortRef operator<<(PortName<Name, NamedPortKind::event>, R&& rhs)
    {
        return _materialize_node_ref(std::forward<R>(rhs)).event_port(Name.view());
    }

    template<class L, fixed_string Name>
    requires NodeLike<L>
    EventPortRef operator>>(L&& lhs, PortName<Name, NamedPortKind::event>)
    {
        return _materialize_node_ref(std::forward<L>(lhs)).event_port(Name.view());
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

        return g.subgraph([&] {
            auto const midi = g.event_input("midi", EventTypeId::midi);
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

            auto connect_lane_outputs = [&](auto voice_index_c, NodeRef const& voice) {
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

                NodeRef voice = g.subgraph([&] {
                    auto const voice_midi = g.event_input("midi", EventTypeId::midi);
                    auto midi_driver = g.node<MidiVoiceAllocator<voice_index, voice_count>>();
                    midi_driver.connect_event_input("midi", voice_midi);
                    NodeRef voice_node = make_voice(std::move(midi_driver)).node_ref();
                    auto const& voice_impl = voice_node.node();
                    std::vector<OutputRefConfig> voice_outputs;
                    voice_outputs.reserve(voice_impl.outputs().size());
                    for (size_t output_port = 0; output_port < voice_impl.outputs().size(); ++output_port) {
                        voice_outputs.push_back(OutputRefConfig {
                            .ref = voice_node[output_port],
                            .config = voice_impl.outputs()[output_port],
                        });
                    }
                    g.outputs(std::span<OutputRefConfig const>(voice_outputs.data(), voice_outputs.size()));

                    std::vector<EventOutputRefConfig> voice_event_outputs;
                    voice_event_outputs.reserve(voice_impl.event_outputs().size());
                    for (size_t output_port = 0; output_port < voice_impl.event_outputs().size(); ++output_port) {
                        voice_event_outputs.push_back(EventOutputRefConfig {
                            .ref = voice_node.event_port(output_port),
                            .config = voice_impl.event_outputs()[output_port],
                        });
                    }
                    g.event_outputs(std::span<EventOutputRefConfig const>(voice_event_outputs.data(), voice_event_outputs.size()));
                }, "PolyphonicVoice");
                voice.connect_event_input("midi", midi);
                auto const& voice_node = voice.node();
                if (
                    voice_node.materialization.is_placeholder()
                    && !voice_node.lowered_subgraph.active()
                ) {
                    details::error("iv::polyphonic callback must return a materialized node or subgraph node");
                }

                if constexpr (voice_index == 0) {
                    output_configs = voice_node.outputs();
                    event_output_configs = voice_node.event_outputs();
                    mix = g.node<PolyphonicMix>(
                        voice_count,
                        output_configs,
                        event_output_configs
                    ).node_ref();
                } else {
                    if (voice_node.outputs().size() != output_configs.size()) {
                        details::error("iv::polyphonic voice lanes must expose the same number of sample outputs");
                    }
                    for (size_t i = 0; i < output_configs.size(); ++i) {
                        if (!same_output_config(voice_node.outputs()[i], output_configs[i])) {
                            details::error("iv::polyphonic voice lanes must expose matching sample output configs");
                        }
                    }

                    if (voice_node.event_outputs().size() != event_output_configs.size()) {
                        details::error("iv::polyphonic voice lanes must expose the same number of event outputs");
                    }
                    for (size_t i = 0; i < event_output_configs.size(); ++i) {
                        if (!same_event_output_config(voice_node.event_outputs()[i], event_output_configs[i])) {
                            details::error("iv::polyphonic voice lanes must expose matching event output configs");
                        }
                    }
                }

                connect_lane_outputs(voice_index_c, voice);
            };

            [&]<size_t... VoiceIndices>(std::index_sequence<VoiceIndices...>) {
                (process_lane(std::integral_constant<size_t, VoiceIndices>{}), ...);
            }(std::make_index_sequence<voice_count>{});

            std::vector<OutputRefConfig> outputs;
            outputs.reserve(output_configs.size());
            for (size_t output_port = 0; output_port < output_configs.size(); ++output_port) {
                outputs.push_back(OutputRefConfig {
                    .ref = mix[output_port],
                    .config = output_configs[output_port],
                });
            }
            g.outputs(std::span<OutputRefConfig const>(outputs.data(), outputs.size()));

            std::vector<EventOutputRefConfig> event_outputs;
            event_outputs.reserve(event_output_configs.size());
            for (size_t output_port = 0; output_port < event_output_configs.size(); ++output_port) {
                event_outputs.push_back(EventOutputRefConfig {
                    .ref = mix.event_port(output_port),
                    .config = event_output_configs[output_port],
                });
            }
            g.event_outputs(std::span<EventOutputRefConfig const>(event_outputs.data(), event_outputs.size()));
        }, "Polyphonic");
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
