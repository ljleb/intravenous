#pragma once

#ifdef IV_INTERNAL_TRANSLATION_UNIT
#error "dsl.h is reserved for user-authored DSL code; include graph/builder.h or module/module.h from internal code."
#endif

#include <intravenous/basic_nodes/midi.h>
#include <intravenous/channel_ports.h>
#include <intravenous/module/module.h>

namespace iv {
    struct PublicOutputSourceSpan {
        std::string_view file_path;
        uint32_t begin;
        uint32_t end;
    };

    inline SourceInfo _public_output_source_info(PublicOutputSourceSpan span)
    {
        return SourceInfo{
            .declaration_identity = "public-output:" + std::string(span.file_path) + ":"
                + std::to_string(span.begin) + ":" + std::to_string(span.end),
            .span = SourceSpan{ .file_path = std::string(span.file_path), .begin = span.begin, .end = span.end },
        };
    }

    template<class Fn>
    void _define_public_sample_outputs_with_source_info(
        GraphBuilder& builder, std::initializer_list<PublicOutputSourceSpan> spans, Fn&& define)
    {
        std::forward<Fn>(define)();
        std::vector<SourceInfo> infos;
        infos.reserve(spans.size());
        for (auto span : spans) infos.push_back(_public_output_source_info(span));
        builder.annotate_public_sample_output_source_info(infos);
    }

    template<class Fn>
    void _define_public_event_outputs_with_source_info(
        GraphBuilder& builder, std::initializer_list<PublicOutputSourceSpan> spans, Fn&& define)
    {
        std::forward<Fn>(define)();
        std::vector<SourceInfo> infos;
        infos.reserve(spans.size());
        for (auto span : spans) infos.push_back(_public_output_source_info(span));
        builder.annotate_public_event_output_source_info(infos);
    }

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

    inline PublicEventInputRef&& _annotate_public_event_input_source_info(
        PublicEventInputRef&& ref, std::string_view declaration_identity,
        std::string_view file_path, uint32_t begin, uint32_t end)
    {
        ref._annotate_source_info(declaration_identity, file_path, begin, end);
        return std::move(ref);
    }

    inline PublicEventInputRef& _annotate_public_event_input_source_info(
        PublicEventInputRef& ref, std::string_view declaration_identity,
        std::string_view file_path, uint32_t begin, uint32_t end)
    {
        ref._annotate_source_info(declaration_identity, file_path, begin, end);
        return ref;
    }

    inline PublicEventInputRef const& _annotate_public_event_input_source_info(
        PublicEventInputRef const& ref, std::string_view declaration_identity,
        std::string_view file_path, uint32_t begin, uint32_t end)
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
    struct typed_sample_port_traits {};

    template<class ChannelType, SampleStreamLayout Layout>
    struct typed_sample_port_traits<TypedSamplePortRef<ChannelType, Layout>> {
        using channel_type = ChannelType;
        static constexpr auto sample_layout = Layout;
    };

    template<class T>
    concept TypedSamplePortLike = requires {
        typename typed_sample_port_traits<std::remove_cvref_t<T>>::channel_type;
    };

    template<class L, class R, bool HasLeft = TypedSamplePortLike<L>>
    struct binary_typed_channel;

    template<class L, class R>
    struct binary_typed_channel<L, R, true> {
        using type = typename typed_sample_port_traits<std::remove_cvref_t<L>>::channel_type;
    };

    template<class L, class R>
    struct binary_typed_channel<L, R, false> {
        using type = typename typed_sample_port_traits<std::remove_cvref_t<R>>::channel_type;
    };

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
    auto make_binary_op(L&& lhs, R&& rhs, std::string_view op_name)
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
    requires (
        (SamplePortLike<L> || ScalarLike<L>) &&
        (SamplePortLike<R> || ScalarLike<R>) &&
        !(TypedSamplePortLike<L> || TypedSamplePortLike<R>))
    NodeRef operator+(L&& lhs, R&& rhs)
    {
        return make_binary_op<Sum<mono, SampleStreamLayout::planar, 2>>(
            std::forward<L>(lhs),
            std::forward<R>(rhs),
            "operator+"
        );
    }

    template<class L, class R>
    requires (
        (TypedSamplePortLike<L> || ScalarLike<L>) &&
        (TypedSamplePortLike<R> || ScalarLike<R>) &&
        (TypedSamplePortLike<L> || TypedSamplePortLike<R>))
    auto operator+(L&& lhs, R&& rhs)
    {
        using Left = typed_sample_port_traits<std::remove_cvref_t<L>>;
        using Right = typed_sample_port_traits<std::remove_cvref_t<R>>;

        if constexpr (TypedSamplePortLike<L> && TypedSamplePortLike<R>) {
            static_assert(
                std::same_as<typename Left::channel_type, typename Right::channel_type>,
                "operator+: typed sample ports must have the same channel type");
        }

        using ChannelType = typename binary_typed_channel<L, R>::type;
        constexpr auto layout = [] {
            if constexpr (TypedSamplePortLike<L> && TypedSamplePortLike<R>) {
                return Left::sample_layout == Right::sample_layout
                    ? Left::sample_layout
                    : SampleStreamLayout::planar;
            } else if constexpr (TypedSamplePortLike<L>) {
                return Left::sample_layout;
            } else {
                return Right::sample_layout;
            }
        }();

        auto sum = make_binary_op<Sum<ChannelType, layout, 2>>(
            std::forward<L>(lhs), std::forward<R>(rhs), "operator+");
        return sum[PortName<"out", NamedPortKind::sample>{}];
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
        !std::same_as<std::remove_cvref_t<T>, SamplePortRef> &&
        !TypedSamplePortLike<T>
    )
    SamplePortRef operator~(T&& value)
    {
        return static_cast<SamplePortRef>(std::forward<T>(value)).detach();
    }

    template<class ChannelType, SampleStreamLayout Layout>
    TypedSamplePortRef<ChannelType, Layout> operator~(
        TypedSamplePortRef<ChannelType, Layout> const& value)
    {
        return TypedSamplePortRef<ChannelType, Layout>{value.erased().detach()};
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

    template<size_t I, class Node>
    auto get(StructuredNodeRef<Node> const& node_ref)
    {
        return node_ref.template get<I>();
    }

    template<size_t I, class Node>
    auto get(StructuredNodeRef<Node>& node_ref)
    {
        return node_ref.template get<I>();
    }

    template<size_t I, class Node>
    auto get(StructuredNodeRef<Node>&& node_ref)
    {
        return node_ref.template get<I>();
    }

    template<size_t voice_count, class Fn>
    void polyphonic(GraphBuilder& g, Fn&& make_voice)
    {
        static_assert(voice_count > 0, "iv::polyphonic requires at least one voice");

        auto const midi = g.event_input<"midi">(EventTypeId::midi);
        auto process_lane = [&]<size_t VoiceIndex>() {
            auto voice = g.subgraph([&] {
                auto const voice_midi = g.event_input<"midi">(EventTypeId::midi);
                auto midi_driver = g.node<MidiVoiceAllocator<VoiceIndex, voice_count>>();
                midi_driver.connect_event_input("midi", voice_midi);
                static_assert(requires {
                    make_voice.template operator()<VoiceIndex>(std::move(midi_driver));
                }, "iv::polyphonic callback must accept a compile-time voice index and its MIDI driver");
                make_voice.template operator()<VoiceIndex>(std::move(midi_driver));
            }, "PolyphonicVoice");
            voice.connect_event_input("midi", midi);
        };

        [&]<size_t... VoiceIndices>(std::index_sequence<VoiceIndices...>) {
            (process_lane.template operator()<VoiceIndices>(), ...);
        }(std::make_index_sequence<voice_count>{});
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
