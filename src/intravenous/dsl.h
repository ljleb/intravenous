#pragma once

#include "intravenous/graph/builder/subgraphs.hpp"
#ifdef IV_INTERNAL_TRANSLATION_UNIT
#error "dsl.h is reserved for user-authored DSL code; include graph/builder.h or module/abi.h from internal code."
#endif

#include <intravenous/basic_nodes/midi.h>
#include <intravenous/channel_ports.h>
#include <intravenous/module/authoring.h>

namespace iv {
    struct PublicOutputSourceSpan {
        std::string_view file_path;
        uint32_t begin;
        uint32_t end;
    };

    namespace details {
        constexpr void append_dsl_source_position(std::string& result, uint32_t value)
        {
            char digits[11] {};
            size_t begin = sizeof(digits);
            do {
                digits[--begin] = static_cast<char>('0' + value % 10);
                value /= 10;
            } while (value != 0);
            result.append(digits + begin, digits + sizeof(digits));
        }
    }

    constexpr SourceInfo _public_output_source_info(PublicOutputSourceSpan span)
    {
        std::string declaration_identity = "public-output:";
        declaration_identity += span.file_path;
        declaration_identity += ':';
        details::append_dsl_source_position(declaration_identity, span.begin);
        declaration_identity += ':';
        details::append_dsl_source_position(declaration_identity, span.end);
        return SourceInfo{
            .declaration_identity = std::move(declaration_identity),
            .span = SourceSpan{ .file_path = std::string(span.file_path), .begin = span.begin, .end = span.end },
        };
    }

    template<class Fn>
    constexpr void _define_public_sample_outputs_with_source_info(
        GraphBuilder& builder, std::initializer_list<PublicOutputSourceSpan> spans, Fn&& define)
    {
        std::forward<Fn>(define)();
        std::vector<SourceInfo> infos;
        infos.reserve(spans.size());
        for (auto span : spans) infos.push_back(_public_output_source_info(span));
        builder.annotate_public_sample_output_source_info(infos);
    }

    template<class Fn>
    constexpr void _define_public_event_outputs_with_source_info(
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
        ref._annotate_source_info(declaration_identity, std::string_view {}, 0u, 0u);
    };

    template<class Ref>
    requires SourceInfoAnnotatableRef<Ref>
    constexpr Ref&& _annotate_node_source_info(
        Ref&& ref,
        std::string_view declaration_identity
    )
    {
        ref._annotate_source_info(declaration_identity, std::string_view {}, 0u, 0u);
        return std::forward<Ref>(ref);
    }

    constexpr PublicSampleInputRef&& _annotate_public_input_source_info(
        PublicSampleInputRef&& ref,
        std::string_view declaration_identity,
        std::string_view file_path,
        uint32_t begin,
        uint32_t end)
    {
        ref._annotate_source_info(declaration_identity, file_path, begin, end);
        return std::move(ref);
    }

    constexpr PublicSampleInputRef& _annotate_public_input_source_info(
        PublicSampleInputRef& ref,
        std::string_view declaration_identity,
        std::string_view file_path,
        uint32_t begin,
        uint32_t end)
    {
        ref._annotate_source_info(declaration_identity, file_path, begin, end);
        return ref;
    }

    constexpr PublicSampleInputRef const& _annotate_public_input_source_info(
        PublicSampleInputRef const& ref,
        std::string_view declaration_identity,
        std::string_view file_path,
        uint32_t begin,
        uint32_t end)
    {
        ref._annotate_source_info(declaration_identity, file_path, begin, end);
        return ref;
    }

    constexpr PublicEventInputRef&& _annotate_public_event_input_source_info(
        PublicEventInputRef&& ref, std::string_view declaration_identity,
        std::string_view file_path, uint32_t begin, uint32_t end)
    {
        ref._annotate_source_info(declaration_identity, file_path, begin, end);
        return std::move(ref);
    }

    constexpr PublicEventInputRef& _annotate_public_event_input_source_info(
        PublicEventInputRef& ref, std::string_view declaration_identity,
        std::string_view file_path, uint32_t begin, uint32_t end)
    {
        ref._annotate_source_info(declaration_identity, file_path, begin, end);
        return ref;
    }

    constexpr PublicEventInputRef const& _annotate_public_event_input_source_info(
        PublicEventInputRef const& ref, std::string_view declaration_identity,
        std::string_view file_path, uint32_t begin, uint32_t end)
    {
        ref._annotate_source_info(declaration_identity, file_path, begin, end);
        return ref;
    }

    template<class Ref>
    requires SourceInfoAnnotatableRef<Ref>
    constexpr Ref&& _annotate_node_source_info(
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

    template<class Ref>
    constexpr void _annotate_source_info_after_statement(
        Ref* ref,
        char const* declaration_identity,
        char const* file_path,
        uint32_t begin,
        uint32_t end)
    {
        using Value = std::remove_cv_t<Ref>;
        if constexpr (SourceInfoAnnotatableRef<Value>) {
            ref->_annotate_source_info(
                declaration_identity, file_path, begin, end);
        } else if constexpr (std::same_as<Value, PublicSampleInputRef>
                             || std::same_as<Value, PublicEventInputRef>) {
            ref->_annotate_source_info(
                declaration_identity, file_path, begin, end);
        } else {
            static_assert(false, "unsupported source-annotatable DSL reference");
        }
    }

    constexpr void _annotate_public_output_after_statement(
        GraphBuilder* builder,
        bool event,
        size_t ordinal,
        char const* file_path,
        uint32_t begin,
        uint32_t end)
    {
        auto info = _public_output_source_info({file_path, begin, end});
        if (event) {
            builder->annotate_public_event_output_source_info(
                ordinal, std::move(info));
        } else {
            builder->annotate_public_sample_output_source_info(
                ordinal, std::move(info));
        }
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

    consteval SamplePortRef lift(GraphBuilder& g, Sample value)
    {
        return g.node<Constant>(value);
    }

    consteval SamplePortRef lift(SamplePortRef s)
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

    template<class ChannelType>
    struct typed_sample_port_traits<TypedSamplePortRef<ChannelType>> {
        using channel_type = ChannelType;
    };

    template<class ChannelType>
    struct typed_sample_port_traits<TypedSamplePortTileRef<ChannelType>> {
        using channel_type = ChannelType;
    };

    template<class Node>
    requires (details::static_output_count_v<std::remove_cvref_t<Node>> == 1)
    struct typed_sample_port_traits<
        TypedNodeRef<Node, ConcretePortProjection>> {
        static constexpr auto output_layout =
            details::static_output_port_layout_at<std::remove_cvref_t<Node>, 0>();
        using channel_type = typename RuntimeChannelTypeTraits<
            output_layout.channel_type>::type;
    };

    template<class Node, class ChannelType>
    requires (details::static_output_count_v<std::remove_cvref_t<Node>> == 1)
    struct typed_sample_port_traits<
        TypedNodeRef<Node, TiledPortProjection<ChannelType>>> {
        using channel_type = ChannelType;
    };

    template<class T>
    concept TypedSamplePortLike = requires {
        typename typed_sample_port_traits<std::remove_cvref_t<T>>::channel_type;
    };

    template<class L, class R,
             bool HasLeft = TypedSamplePortLike<L>,
             bool HasRight = TypedSamplePortLike<R>>
    struct binary_typed_channel;

    template<class L, class R>
    struct binary_typed_channel<L, R, true, false> {
        using type =
            typename typed_sample_port_traits<std::remove_cvref_t<L>>::channel_type;
    };

    template<class L, class R>
    struct binary_typed_channel<L, R, false, true> {
        using type =
            typename typed_sample_port_traits<std::remove_cvref_t<R>>::channel_type;
    };

    template<class L, class R>
    struct binary_typed_channel<L, R, true, true> {
        using left_type =
            typename typed_sample_port_traits<std::remove_cvref_t<L>>::channel_type;
        using right_type =
            typename typed_sample_port_traits<std::remove_cvref_t<R>>::channel_type;
        using type = std::conditional_t<
            std::same_as<left_type, mono>,
            right_type,
            left_type>;

        static_assert(
            std::same_as<left_type, right_type> ||
            std::same_as<left_type, mono> ||
            std::same_as<right_type, mono>,
            "typed sample ports must have matching channel types, except that mono broadcasts");
    };

    template<class T>
    concept EventPortLike = std::convertible_to<std::remove_cvref_t<T>, EventPortRef>;

    template<class T>
    concept NodeLike = requires(std::remove_cvref_t<T> const& ref) {
        { ref.node_ref() } -> std::same_as<NodeRef>;
    };

    template<class T>
    requires NodeLike<T>
    consteval NodeRef _materialize_node_ref(T&& value)
    {
        return value.node_ref();
    }

    template<class T>
    consteval EventPortRef lift_event_operand(T&& x)
    {
        if constexpr (EventPortLike<T>) {
            return std::forward<T>(x);
        } else if constexpr (NodeLike<T>) {
            return _materialize_node_ref(std::forward<T>(x)).event_port();
        }
    }

    template<class T>
    consteval SamplePortRef lift_sample_operand(GraphBuilder& g, T&& x)
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

    template<class Node, class ChannelType = void, class L, class R>
    requires ((SamplePortLike<L> || ScalarLike<L>) && (SamplePortLike<R> || ScalarLike<R>))
    consteval auto make_binary_op(L&& lhs, R&& rhs, std::string_view op_name)
    {
        GraphBuilder* g = nullptr;

        if constexpr (SamplePortLike<L>) {
            SamplePortRef s = static_cast<SamplePortRef>(lhs);
            g = s.graph_builder;
        }

        if constexpr (SamplePortLike<R>) {
            SamplePortRef s = static_cast<SamplePortRef>(rhs);
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

        if constexpr (std::same_as<ChannelType, void> ||
                      std::same_as<ChannelType, mono>) {
            return g->node<Node>()(lhs_sample_port, rhs_sample_port);
        } else {
            return g->node<Node, ChannelType>()(lhs_sample_port, rhs_sample_port);
        }
    }

    template<class L, class R>
    requires (
        (SamplePortLike<L> || ScalarLike<L>) &&
        (SamplePortLike<R> || ScalarLike<R>) &&
        !(TypedSamplePortLike<L> || TypedSamplePortLike<R>))
    consteval NodeRef operator+(L&& lhs, R&& rhs)
    {
        return make_binary_op<Sum<mono, SampleStreamLayout::planar, 2>>(
            std::forward<L>(lhs),
            std::forward<R>(rhs),
            "operator+"
        );
    }

    template<class L, class R>
    requires (
        (SamplePortLike<L> || ScalarLike<L>) &&
        (SamplePortLike<R> || ScalarLike<R>) &&
        (TypedSamplePortLike<L> || TypedSamplePortLike<R>))
    consteval auto operator+(L&& lhs, R&& rhs)
    {
        using ChannelType = typename binary_typed_channel<L, R>::type;
        auto sum = make_binary_op<
            Sum<mono, SampleStreamLayout::planar, 2>, ChannelType>(
            std::forward<L>(lhs), std::forward<R>(rhs), "operator+");
        return sum;
    }

    template<class L, class R>
    requires ((SamplePortLike<L> || ScalarLike<L>) && (SamplePortLike<R> || ScalarLike<R>))
    consteval NodeRef operator-(L&& lhs, R&& rhs)
    {
        return make_binary_op<Subtract>(
            std::forward<L>(lhs),
            std::forward<R>(rhs),
            "operator-"
        );
    }

    template<class L, class R>
    requires (
        (SamplePortLike<L> || ScalarLike<L>) &&
        (SamplePortLike<R> || ScalarLike<R>) &&
        !(TypedSamplePortLike<L> || TypedSamplePortLike<R>))
    consteval NodeRef operator*(L&& lhs, R&& rhs)
    {
        return make_binary_op<Product<2>>(
            std::forward<L>(lhs),
            std::forward<R>(rhs),
            "operator*"
        );
    }

    template<class L, class R>
    requires (
        (SamplePortLike<L> || ScalarLike<L>) &&
        (SamplePortLike<R> || ScalarLike<R>) &&
        (TypedSamplePortLike<L> || TypedSamplePortLike<R>))
    consteval auto operator*(L&& lhs, R&& rhs)
    {
        using ChannelType = typename binary_typed_channel<L, R>::type;
        auto product = make_binary_op<Product<2>, ChannelType>(
            std::forward<L>(lhs), std::forward<R>(rhs), "operator*");
        return product;
    }

    template<class L, class R>
    requires ((SamplePortLike<L> || ScalarLike<L>) && (SamplePortLike<R> || ScalarLike<R>))
    consteval NodeRef operator/(L&& lhs, R&& rhs)
    {
        return make_binary_op<Quotient>(
            std::forward<L>(lhs),
            std::forward<R>(rhs),
            "operator/"
        );
    }

    consteval SamplePortRef operator~(SamplePortRef const& sample_port)
    {
        return sample_port.detach();
    }

    consteval SamplePortRef operator~(SamplePortRef&& sample_port)
    {
        return sample_port.detach();
    }

    template<class T>
    requires (
        std::convertible_to<std::remove_cvref_t<T>, SamplePortRef> &&
        !std::same_as<std::remove_cvref_t<T>, SamplePortRef> &&
        !TypedSamplePortLike<T>
    )
    consteval SamplePortRef operator~(T&& value)
    {
        return static_cast<SamplePortRef>(std::forward<T>(value)).detach();
    }

    template<class ChannelType>
    consteval TypedSamplePortRef<ChannelType> operator~(
        TypedSamplePortRef<ChannelType> const& value)
    {
        return TypedSamplePortRef<ChannelType>{value.erased().detach()};
    }

    template<class ChannelType>
    consteval TypedSamplePortRef<ChannelType> operator~(
        TypedSamplePortTileRef<ChannelType> const& value)
    {
        return TypedSamplePortRef<ChannelType>{
            static_cast<SamplePortRef>(value).detach()};
    }

    template<class L, class R>
    requires (
        SamplePortLike<L> &&
        NodeLike<R>
    )
    consteval auto connect_unary_node(L&& lhs, R&& rhs, std::string_view op_name)
    {
        SamplePortRef source = static_cast<SamplePortRef>(std::forward<L>(lhs));
        NodeRef target = _materialize_node_ref(std::forward<R>(rhs));

        auto const input_count = target.sample_input_count();
        auto const output_count = target.sample_output_count();

        if (input_count != 1) {
            details::error(
                std::string(op_name) + " requires target to have exactly 1 input; got " +
                std::to_string(input_count) + " inputs on " + target.to_string()
            );
        }

        target.connect_input(0, source);
        if (output_count == 0) {
            if constexpr (NodeLike<L>) {
                return target;
            } else {
                return source;
            }
        }
        if (output_count != 1) {
            details::error(
                std::string(op_name) + " requires target to have at most 1 output when used as an expression; got " +
                std::to_string(output_count) + " outputs on " + target.to_string()
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
    consteval auto operator>(L&& lhs, R&& rhs)
    {
        return connect_unary_node(std::forward<L>(lhs), std::forward<R>(rhs), "operator>");
    }

    template<class L, class R>
    requires (
        NodeLike<L> &&
        SamplePortLike<R>
    )
    consteval auto operator<(L&& lhs, R&& rhs)
    {
        return connect_unary_node(std::forward<R>(rhs), std::forward<L>(lhs), "operator<");
    }

    template<class L, class R>
    requires (
        EventPortLike<L> &&
        NodeLike<R>
    )
    consteval auto connect_unary_event_node(L&& lhs, R&& rhs, std::string_view op_name)
    {
        EventPortRef source = std::forward<L>(lhs);
        NodeRef target = _materialize_node_ref(std::forward<R>(rhs));

        auto const event_input_count = target.event_input_count();
        auto const event_output_count = target.event_output_count();

        if (event_input_count != 1) {
            details::error(
                std::string(op_name) + " requires target to have exactly 1 event input; got " +
                std::to_string(event_input_count) + " event inputs on " + target.to_string()
            );
        }

        target.connect_event_input(0, source);
        if (event_output_count == 0) {
            return source;
        }
        if (event_output_count != 1) {
            details::error(
                std::string(op_name) + " requires target to have at most 1 event output when used as an expression; got " +
                std::to_string(event_output_count) + " event outputs on " + target.to_string()
            );
        }
        return target.event_port(0);
    }

    template<class L, class R>
    requires (
        EventPortLike<L> &&
        NodeLike<R>
    )
    consteval auto operator>(L&& lhs, R&& rhs)
    {
        return connect_unary_event_node(std::forward<L>(lhs), std::forward<R>(rhs), "operator>");
    }

    template<class L, class R>
    requires (
        NodeLike<L> &&
        EventPortLike<R>
    )
    consteval auto operator<(L&& lhs, R&& rhs)
    {
        return connect_unary_event_node(
            std::forward<R>(rhs),
            _materialize_node_ref(std::forward<L>(lhs)),
            "operator<"
        );
    }

    template<size_t I, class Node, class PortProjection>
    consteval auto get(TypedNodeRef<Node, PortProjection> const& node_ref)
    {
        return node_ref.template get<I>();
    }

    template<size_t I, class Node, class PortProjection>
    consteval auto get(TypedNodeRef<Node, PortProjection>& node_ref)
    {
        return node_ref.template get<I>();
    }

    template<size_t I, class Node, class PortProjection>
    consteval auto get(TypedNodeRef<Node, PortProjection>&& node_ref)
    {
        return node_ref.template get<I>();
    }

    template<size_t voice_count, class Fn>
    consteval void polyphonic(GraphBuilder& g, Fn&& make_voice)
    {
        static_assert(voice_count > 0, "iv::polyphonic requires at least one voice");

        auto const midi = g.event_input<"midi">(EventTypeId::midi);
        auto process_lane = [&]<size_t VoiceIndex>() {
            auto voice = g.subgraph([&](auto& s){
                auto const voice_midi = s.template event_input<"midi">(EventTypeId::midi);
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
    template<class Node, class PortProjection>
    struct tuple_size<iv::TypedNodeRef<Node, PortProjection>> :
        std::integral_constant<size_t, iv::details::static_output_count_v<Node>> {};

    template<size_t I, class Node, class PortProjection>
    struct tuple_element<I, iv::TypedNodeRef<Node, PortProjection>> {
        using type = iv::SamplePortRef;
    };
}
