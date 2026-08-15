#pragma once

#include <array>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <utility>

namespace iv {
    // This is the sole inventory of supported channel types and their named
    // members.  The expansion below derives the static descriptors and the
    // runtime IDs; do not add parallel type/member registries.
#define IV_CHANNEL_TYPES(X) \
    X(mono, "mono", center) \
    X(stereo, "stereo", left, right)

    template<class Member, class T>
    constexpr auto make_channel_assignment(Member member, T&& value);

    template<class Type, class Tag>
    struct ChannelMember {
        using channel_type = Type;
        using tag_type = Tag;

        static consteval size_t ordinal()
        {
            for (size_t i = 0; i < Type::channel_names.size(); ++i) {
                if (Type::channel_names[i] == Tag::name) return i;
            }
            throw "channel member is not registered by its channel type";
        }

        static constexpr size_t channel_ordinal = ordinal();

        template<class T>
        constexpr auto operator=(T&& value) const
        {
            return make_channel_assignment(*this, std::forward<T>(value));
        }
    };

    template<class LeftType, class LeftTag, class RightType, class RightTag>
    constexpr bool operator==(
        ChannelMember<LeftType, LeftTag>,
        ChannelMember<RightType, RightTag>)
    {
        return std::same_as<LeftType, RightType> && std::same_as<LeftTag, RightTag>;
    }

    template<class... Types>
    struct ChannelTypeList {};

    template<class... Members>
    struct ChannelMemberList {};

    template<class Type>
    struct ChannelTypeTag { using type = Type; };

    template<class... Tags>
    consteval auto channel_type_list_from_tags(std::tuple<Tags...>)
        -> ChannelTypeList<typename Tags::type...>;

    enum class ChannelTypeId : std::uint8_t {
#define IV_CHANNEL_TYPE_ID(name, wire_name, ...) name,
        IV_CHANNEL_TYPES(IV_CHANNEL_TYPE_ID)
#undef IV_CHANNEL_TYPE_ID
        count,
    };

    template<class Type>
    struct ChannelTypeTraits;

    template<ChannelTypeId Id>
    struct RuntimeChannelTypeTraits;

// The registry intentionally has a small bounded channel arity.  Extending
// these helpers is mechanical, while each channel name remains authored once.
#define IV_PP_CAT_I(a, b) a##b
#define IV_PP_CAT(a, b) IV_PP_CAT_I(a, b)
#define IV_PP_NARG_I(_1, _2, _3, _4, _5, _6, _7, _8, count, ...) count
#define IV_PP_NARG(...) IV_PP_NARG_I(__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1)
#define IV_PP_FOR_EACH(m, ...) IV_PP_CAT(IV_PP_FOR_EACH_, IV_PP_NARG(__VA_ARGS__))(m, __VA_ARGS__)
#define IV_PP_FOR_EACH_1(m, a) m(a)
#define IV_PP_FOR_EACH_2(m, a, b) m(a) m(b)
#define IV_PP_FOR_EACH_3(m, a, b, c) m(a) m(b) m(c)
#define IV_PP_FOR_EACH_4(m, a, b, c, d) m(a) m(b) m(c) m(d)
#define IV_PP_FOR_EACH_5(m, a, b, c, d, e) m(a) m(b) m(c) m(d) m(e)
#define IV_PP_FOR_EACH_6(m, a, b, c, d, e, f) m(a) m(b) m(c) m(d) m(e) m(f)
#define IV_PP_FOR_EACH_7(m, a, b, c, d, e, f, g) m(a) m(b) m(c) m(d) m(e) m(f) m(g)
#define IV_PP_FOR_EACH_8(m, a, b, c, d, e, f, g, h) m(a) m(b) m(c) m(d) m(e) m(f) m(g) m(h)
#define IV_PP_FOR_EACH_COMMA(m, ...) IV_PP_CAT(IV_PP_FOR_EACH_COMMA_, IV_PP_NARG(__VA_ARGS__))(m, __VA_ARGS__)
#define IV_PP_FOR_EACH_COMMA_1(m, a) m(a)
#define IV_PP_FOR_EACH_COMMA_2(m, a, b) m(a), m(b)
#define IV_PP_FOR_EACH_COMMA_3(m, a, b, c) m(a), m(b), m(c)
#define IV_PP_FOR_EACH_COMMA_4(m, a, b, c, d) m(a), m(b), m(c), m(d)
#define IV_PP_FOR_EACH_COMMA_5(m, a, b, c, d, e) m(a), m(b), m(c), m(d), m(e)
#define IV_PP_FOR_EACH_COMMA_6(m, a, b, c, d, e, f) m(a), m(b), m(c), m(d), m(e), m(f)
#define IV_PP_FOR_EACH_COMMA_7(m, a, b, c, d, e, f, g) m(a), m(b), m(c), m(d), m(e), m(f), m(g)
#define IV_PP_FOR_EACH_COMMA_8(m, a, b, c, d, e, f, g, h) m(a), m(b), m(c), m(d), m(e), m(f), m(g), m(h)

#define IV_CHANNEL_NAME(channel) std::string_view{#channel}
#define IV_CHANNEL_TAG(channel) struct IV_PP_CAT(channel, _tag) { static constexpr std::string_view name = #channel; };
#define IV_CHANNEL_MEMBER_TYPE(channel) ChannelMember<self, IV_PP_CAT(channel, _tag)>
#define IV_CHANNEL_MEMBER(channel) static inline constexpr ChannelMember<self, IV_PP_CAT(channel, _tag)> channel{};
#define IV_DEFINE_CHANNEL_TYPE(name, wire, ...) \
    struct name { \
        using self = name; \
        IV_PP_FOR_EACH(IV_CHANNEL_TAG, __VA_ARGS__) \
        static constexpr std::string_view wire_name = wire; \
        static constexpr auto channel_names = std::array{IV_PP_FOR_EACH_COMMA(IV_CHANNEL_NAME, __VA_ARGS__)}; \
        static constexpr size_t channel_count = channel_names.size(); \
        using members = ChannelMemberList<IV_PP_FOR_EACH_COMMA(IV_CHANNEL_MEMBER_TYPE, __VA_ARGS__)>; \
        IV_PP_FOR_EACH(IV_CHANNEL_MEMBER, __VA_ARGS__) \
    }; \
    template<> struct ChannelTypeTraits<name> { \
        static constexpr ChannelTypeId id = ChannelTypeId::name; \
        static constexpr std::string_view wire_name = wire; \
    }; \
    template<> struct RuntimeChannelTypeTraits<ChannelTypeId::name> { \
        using type = name; \
        static constexpr size_t count = type::channel_count; \
        static constexpr std::string_view wire_name = wire; \
    };

    IV_CHANNEL_TYPES(IV_DEFINE_CHANNEL_TYPE)

#undef IV_DEFINE_CHANNEL_TYPE
#undef IV_CHANNEL_MEMBER
#undef IV_CHANNEL_MEMBER_TYPE
#undef IV_CHANNEL_TAG
#undef IV_CHANNEL_NAME
#undef IV_PP_FOR_EACH_COMMA_8
#undef IV_PP_FOR_EACH_COMMA_7
#undef IV_PP_FOR_EACH_COMMA_6
#undef IV_PP_FOR_EACH_COMMA_5
#undef IV_PP_FOR_EACH_COMMA_4
#undef IV_PP_FOR_EACH_COMMA_3
#undef IV_PP_FOR_EACH_COMMA_2
#undef IV_PP_FOR_EACH_COMMA_1
#undef IV_PP_FOR_EACH_COMMA
#undef IV_PP_FOR_EACH_8
#undef IV_PP_FOR_EACH_7
#undef IV_PP_FOR_EACH_6
#undef IV_PP_FOR_EACH_5
#undef IV_PP_FOR_EACH_4
#undef IV_PP_FOR_EACH_3
#undef IV_PP_FOR_EACH_2
#undef IV_PP_FOR_EACH_1
#undef IV_PP_FOR_EACH
#undef IV_PP_NARG
#undef IV_PP_NARG_I
#undef IV_PP_CAT
#undef IV_PP_CAT_I

#define IV_CHANNEL_TYPE_AS_TAG(name, wire_name, ...) ChannelTypeTag<name>{},
    using SupportedChannelTypes = decltype(channel_type_list_from_tags(std::tuple{
        IV_CHANNEL_TYPES(IV_CHANNEL_TYPE_AS_TAG)
    }));
#undef IV_CHANNEL_TYPE_AS_TAG
}
