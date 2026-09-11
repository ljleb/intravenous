#pragma once

#include <intravenous/node/code_key.h>

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace iv::details {
struct ConfigurationArgument {
    void* data = nullptr;
    char const* type_name = nullptr;
    std::size_t type_name_size = 0;
};

template<class T>
constexpr ConfigurationArgument configuration_argument(T& value) noexcept
{
    constexpr auto type_name = clang_type_name<std::remove_cvref_t<T>>();
    return {
        .data = const_cast<void*>(static_cast<void const*>(std::addressof(value))),
        .type_name = type_name.data(),
        .type_name_size = type_name.size(),
    };
}

template<class Tuple, std::size_t... Index>
constexpr auto configuration_arguments(
    Tuple& values, std::index_sequence<Index...>) noexcept
{
    return std::array<ConfigurationArgument, sizeof...(Index)>{
        configuration_argument(std::get<Index>(values))...
    };
}

template<class... T>
constexpr auto configuration_arguments(std::tuple<T...>& values) noexcept
{
    return configuration_arguments(values, std::index_sequence_for<T...>{});
}

inline std::string_view configuration_argument_type(
    ConfigurationArgument const& argument) noexcept
{
    return argument.type_name
        ? std::string_view(argument.type_name, argument.type_name_size)
        : std::string_view{};
}
}
