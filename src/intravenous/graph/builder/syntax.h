#pragma once

#include <intravenous/ports.h>

#include <array>
#include <cstddef>
#include <ranges>
#include <string_view>
#include <type_traits>

namespace iv {
    template<size_t N>
    struct fixed_string {
        char value[N];

        constexpr fixed_string(char const (&str)[N])
        {
            std::ranges::copy(str, value);
        }

        constexpr fixed_string(std::array<char, N> const& chars)
        {
            std::ranges::copy(chars, value);
        }

        constexpr std::string_view view() const
        {
            return std::string_view(value, N - 1);
        }
    };

    template<size_t N>
    fixed_string(char const (&)[N]) -> fixed_string<N>;

    using NamedPortKind = PortKind;

    template<fixed_string Name, class T, NamedPortKind Kind = NamedPortKind::sample>
    struct NamedArg {
        using value_type = T;
        static constexpr auto name = Name;
        static constexpr auto kind = Kind;

        T value;
    };

    // A public-port argument whose name and channel are separate pieces of
    // identity. It intentionally is not a NamedArg: node calls remain mono.
    template<fixed_string Name, class ChannelType, size_t ChannelOrdinal, class T>
    struct ChannelNamedArg {
        using value_type = T;
        static constexpr auto name = Name;
        using channel_type = ChannelType;
        static constexpr size_t channel_ordinal = ChannelOrdinal;

        T value;
    };

    template<fixed_string Name, class ChannelType, size_t ChannelOrdinal>
    struct ChannelPortName {
        template<class T>
        constexpr auto operator=(T&& value) const;
    };

    template<fixed_string Name, NamedPortKind Kind = NamedPortKind::sample>
    struct PortName {
        template<class T>
        constexpr auto operator=(T&& value) const;

        template<class Channel>
        constexpr auto operator[](Channel) const
        {
            using ChannelT = std::remove_cvref_t<Channel>;
            return ChannelPortName<Name, typename ChannelT::channel_type, ChannelT::channel_ordinal>{};
        }
    };

    template<class ChannelType, size_t ChannelOrdinal, class T>
    struct DefaultChannelNamedArg {
        using value_type = T;
        using channel_type = ChannelType;
        static constexpr size_t channel_ordinal = ChannelOrdinal;
        T value;
    };
}
