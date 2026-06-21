#pragma once

#include <intravenous/graph/builder/syntax.h>
#include <intravenous/lane_node/channels.h>

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace iv {
    enum class MonoChannelPortTag {
        center,
    };

    enum class StereoChannelPortTag {
        left,
        right,
    };

    template<ChannelTypeId Type>
    struct ChannelPortTraits;

    template<>
    struct ChannelPortTraits<ChannelTypeId::mono> {
        using tag_type = MonoChannelPortTag;

        static constexpr auto type_name = fixed_string("mono");

        template<tag_type Tag>
        static consteval auto tag_name()
        {
            static_assert(Tag == tag_type::center);
            return fixed_string("center");
        }

        static constexpr tag_type swap_side(tag_type tag)
        {
            return tag;
        }
    };

    template<>
    struct ChannelPortTraits<ChannelTypeId::stereo> {
        using tag_type = StereoChannelPortTag;

        static constexpr auto type_name = fixed_string("stereo");

        template<tag_type Tag>
        static consteval auto tag_name()
        {
            if constexpr (Tag == tag_type::left) {
                return fixed_string("left");
            } else {
                return fixed_string("right");
            }
        }

        static constexpr tag_type swap_side(tag_type tag)
        {
            return tag == tag_type::left ? tag_type::right : tag_type::left;
        }
    };

    namespace details {
        template<size_t Value>
        consteval size_t decimal_digit_count()
        {
            size_t digits = 1;
            size_t remaining = Value;
            while (remaining >= 10) {
                remaining /= 10;
                ++digits;
            }
            return digits;
        }

        template<size_t Value>
        consteval auto decimal_fixed_string()
        {
            constexpr size_t digits = decimal_digit_count<Value>();
            std::array<char, digits + 1> chars {};
            size_t remaining = Value;
            for (size_t i = 0; i < digits; ++i) {
                chars[digits - 1 - i] = static_cast<char>('0' + (remaining % 10));
                remaining /= 10;
            }
            chars[digits] = '\0';
            return fixed_string(chars);
        }

        template<fixed_string A, fixed_string B>
        consteval auto fixed_string_concat()
        {
            std::array<char, A.view().size() + B.view().size() + 1> chars {};
            size_t offset = 0;
            for (char c : A.view()) {
                chars[offset++] = c;
            }
            for (char c : B.view()) {
                chars[offset++] = c;
            }
            chars[offset] = '\0';
            return fixed_string(chars);
        }

        template<fixed_string A, fixed_string B, fixed_string C, fixed_string D, fixed_string E>
        consteval auto fixed_string_concat()
        {
            return fixed_string_concat<
                fixed_string_concat<
                    fixed_string_concat<
                        fixed_string_concat<A, B>(),
                        C>(),
                    D>(),
                E>();
        }
    }

    template<ChannelTypeId Type, auto Tag, size_t Index = 0>
    struct ChannelPortId {
        using traits = ChannelPortTraits<Type>;
        using tag_type = typename traits::tag_type;
        static_assert(std::same_as<std::remove_cv_t<decltype(Tag)>, tag_type>);

        static constexpr auto type = Type;
        static constexpr auto tag = Tag;
        static constexpr size_t index = Index;

        static constexpr auto port_name = details::fixed_string_concat<
            fixed_string("__"),
            traits::type_name,
            fixed_string("_"),
            traits::template tag_name<Tag>(),
            details::fixed_string_concat<
                fixed_string("_"),
                details::decimal_fixed_string<Index>()>()>();

        constexpr operator PortName<port_name, NamedPortKind::sample>() const
        {
            return {};
        }

        template<class T>
        constexpr auto operator=(T&& value) const
        {
            return static_cast<PortName<port_name, NamedPortKind::sample>>(*this) = std::forward<T>(value);
        }
    };

    template<ChannelTypeId Type, auto Tag, size_t Index>
    constexpr size_t port_index(ChannelPortId<Type, Tag, Index>)
    {
        return Index;
    }

    template<size_t NextIndex, ChannelTypeId Type, auto Tag, size_t Index>
    constexpr auto with_port_index(ChannelPortId<Type, Tag, Index>)
    {
        return ChannelPortId<Type, Tag, NextIndex>{};
    }

    template<ChannelTypeId Type, auto Tag, size_t Index>
    constexpr auto swap_side(ChannelPortId<Type, Tag, Index>)
    {
        return ChannelPortId<Type, ChannelPortTraits<Type>::swap_side(Tag), Index>{};
    }

    namespace channels {
        inline constexpr auto mono = ChannelPortId<ChannelTypeId::mono, MonoChannelPortTag::center>{};
        inline constexpr auto stereo_left =
            ChannelPortId<ChannelTypeId::stereo, StereoChannelPortTag::left>{};
        inline constexpr auto stereo_right =
            ChannelPortId<ChannelTypeId::stereo, StereoChannelPortTag::right>{};
    }
}
