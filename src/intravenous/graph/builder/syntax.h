#pragma once

#include <intravenous/ports.h>

#include <array>
#include <cstddef>
#include <ranges>
#include <string_view>

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

    template<fixed_string Name, NamedPortKind Kind = NamedPortKind::sample>
    struct PortName {
        template<class T>
        constexpr auto operator=(T&& value) const;
    };
}
