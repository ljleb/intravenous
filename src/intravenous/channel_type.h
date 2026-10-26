#pragma once

#include <array>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>
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

        static consteval size_t index()
        {
            for (size_t i = 0; i < Type::channel_names.size(); ++i) {
                if (Type::channel_names[i] == Tag::name) return i;
            }
            throw "channel member is not registered by its channel type";
        }

        static constexpr size_t channel_index = index();

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

    struct ChannelTypeInfo {
        ChannelTypeId id {};
        std::string_view wire_name {};
        std::size_t channel_count = 0;
    };

// The registry intentionally has a small bounded channel arity.  Extending
// these helpers is mechanical, while each channel name remains configured once.
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

    inline constexpr auto channel_type_registry = std::array {
#define IV_CHANNEL_TYPE_INFO(name, wire, ...)                            \
        ChannelTypeInfo {                                                \
            .id = ChannelTypeId::name,                                   \
            .wire_name = wire,                                           \
            .channel_count = name::channel_count,                        \
        },
        IV_CHANNEL_TYPES(IV_CHANNEL_TYPE_INFO)
#undef IV_CHANNEL_TYPE_INFO
    };

    static_assert(
        channel_type_registry.size()
        == static_cast<std::size_t>(ChannelTypeId::count));
    static_assert([] {
        for (std::size_t index = 0; index < channel_type_registry.size(); ++index) {
            if (channel_type_registry[index].id
                != static_cast<ChannelTypeId>(index)) {
                return false;
            }
        }
        return true;
    }());

    [[nodiscard]] constexpr bool is_registered_channel_type(
        ChannelTypeId id) noexcept
    {
        return static_cast<std::size_t>(id) < channel_type_registry.size();
    }

    [[nodiscard]] constexpr ChannelTypeInfo const& channel_type_info(
        ChannelTypeId id)
    {
        if (!is_registered_channel_type(id)) {
            throw std::logic_error("invalid channel type");
        }
        return channel_type_registry[static_cast<std::size_t>(id)];
    }

    // A channel conversion is declared as a target-major coefficient matrix.
    // Identity conversions exist for every registered channel type. Every
    // non-identity semantic pair must be declared deliberately; the all-pairs
    // validation below prevents a registry extension from falling through to
    // an invented channel ordering or reduction.
    template<class Source, class Target>
    struct ChannelConversion;

    namespace channel_type_details {
        template<class Type>
        consteval auto identity_conversion()
        {
            std::array<float, Type::channel_count * Type::channel_count> result {};
            for (std::size_t channel = 0; channel < Type::channel_count; ++channel) {
                result[channel * Type::channel_count + channel] = 1.0f;
            }
            return result;
        }
    }

    template<class Type>
    struct ChannelConversion<Type, Type> {
        static constexpr auto coefficients =
            channel_type_details::identity_conversion<Type>();
    };

    template<>
    struct ChannelConversion<mono, stereo> {
        // left = center; right = center
        static constexpr std::array<float, 2> coefficients {1.0f, 1.0f};
    };

    template<>
    struct ChannelConversion<stereo, mono> {
        // center = (left + right) / 2
        static constexpr std::array<float, 2> coefficients {0.5f, 0.5f};
    };

    template<class Source, class Target>
    concept DeclaredChannelConversion = requires {
        ChannelConversion<Source, Target>::coefficients;
    } && (ChannelConversion<Source, Target>::coefficients.size()
        == Source::channel_count * Target::channel_count);

    namespace channel_type_details {
        template<class Source, class... Targets>
        consteval bool source_converts_to_all(ChannelTypeList<Targets...>)
        {
            return (DeclaredChannelConversion<Source, Targets> && ...);
        }

        template<class... Types>
        consteval bool all_conversions_declared(ChannelTypeList<Types...> types)
        {
            return (source_converts_to_all<Types>(types) && ...);
        }

        template<class Source, class Target>
        constexpr float conversion_coefficient(
            std::size_t source_channel,
            std::size_t target_channel)
        {
            if (source_channel >= Source::channel_count
                || target_channel >= Target::channel_count) {
                throw std::logic_error("channel conversion index is out of bounds");
            }
            return ChannelConversion<Source, Target>::coefficients[
                target_channel * Source::channel_count + source_channel];
        }

        template<class Source, class... Targets>
        constexpr float conversion_coefficient_for_target(
            ChannelTypeId target,
            std::size_t source_channel,
            std::size_t target_channel,
            ChannelTypeList<Targets...>)
        {
            float result = 0.0f;
            auto const found = ((target == ChannelTypeTraits<Targets>::id
                    ? (result = conversion_coefficient<Source, Targets>(
                           source_channel, target_channel),
                        true)
                    : false)
                || ...);
            if (!found) throw std::logic_error("invalid target channel type");
            return result;
        }

        template<class... Sources>
        constexpr float conversion_coefficient_for_source(
            ChannelTypeId source,
            ChannelTypeId target,
            std::size_t source_channel,
            std::size_t target_channel,
            ChannelTypeList<Sources...> types)
        {
            float result = 0.0f;
            auto const found = ((source == ChannelTypeTraits<Sources>::id
                    ? (result = conversion_coefficient_for_target<Sources>(
                           target, source_channel, target_channel, types),
                        true)
                    : false)
                || ...);
            if (!found) throw std::logic_error("invalid source channel type");
            return result;
        }
    }

    static_assert(
        channel_type_details::all_conversions_declared(SupportedChannelTypes{}),
        "every ordered pair of registered channel types requires a deliberate conversion declaration");

    [[nodiscard]] constexpr float channel_conversion_coefficient(
        ChannelTypeId source,
        ChannelTypeId target,
        std::size_t source_channel,
        std::size_t target_channel)
    {
        return channel_type_details::conversion_coefficient_for_source(
            source,
            target,
            source_channel,
            target_channel,
            SupportedChannelTypes{});
    }

    [[nodiscard]] constexpr std::optional<std::size_t>
    channel_conversion_aliased_source(
        ChannelTypeId source,
        ChannelTypeId target,
        std::size_t target_channel)
    {
        if (!is_registered_channel_type(source)
            || !is_registered_channel_type(target)
            || target_channel >= channel_type_info(target).channel_count) {
            return std::nullopt;
        }
        std::optional<std::size_t> result;
        for (std::size_t source_channel = 0;
             source_channel < channel_type_info(source).channel_count;
             ++source_channel) {
            auto const coefficient = channel_conversion_coefficient(
                source, target, source_channel, target_channel);
            if (coefficient == 0.0f) continue;
            if (coefficient != 1.0f || result) return std::nullopt;
            result = source_channel;
        }
        return result;
    }

    [[nodiscard]] constexpr bool channel_conversion_target_mixes_sources(
        ChannelTypeId source,
        ChannelTypeId target,
        std::size_t target_channel)
    {
        if (!is_registered_channel_type(source)
            || !is_registered_channel_type(target)
            || target_channel >= channel_type_info(target).channel_count) {
            return false;
        }
        std::size_t participating_sources = 0;
        for (std::size_t source_channel = 0;
             source_channel < channel_type_info(source).channel_count;
             ++source_channel) {
            participating_sources += channel_conversion_coefficient(
                source, target, source_channel, target_channel) != 0.0f;
        }
        return participating_sources > 1;
    }

    [[nodiscard]] constexpr bool channel_conversion_mixes_sources(
        ChannelTypeId source,
        ChannelTypeId target)
    {
        if (!is_registered_channel_type(source)
            || !is_registered_channel_type(target)) {
            return false;
        }
        for (std::size_t target_channel = 0;
             target_channel < channel_type_info(target).channel_count;
             ++target_channel) {
            if (channel_conversion_target_mixes_sources(
                    source, target, target_channel)) {
                return true;
            }
        }
        return false;
    }
}
