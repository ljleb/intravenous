#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace iv {
struct NodeCodeKey {
    std::uint64_t low = 0;
    std::uint64_t high = 0;

    constexpr auto operator<=>(NodeCodeKey const&) const = default;
};

namespace details {
    consteval std::uint64_t fnv1a64(
        std::string_view value,
        std::uint64_t seed)
    {
        std::uint64_t hash = seed;
        for (auto const ch : value) {
            hash ^= static_cast<unsigned char>(ch);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    template<class T>
    consteval std::string_view clang_type_name()
    {
#if defined(__clang__)
        constexpr std::string_view signature = __PRETTY_FUNCTION__;
        constexpr std::string_view prefix = "T = ";
        auto const begin = signature.find(prefix);
        static_assert(begin != std::string_view::npos);
        auto const type_begin = begin + prefix.size();
        auto const end = signature.rfind(']');
        static_assert(end != std::string_view::npos && end > type_begin);
        return signature.substr(type_begin, end - type_begin);
#else
#error "Intravenous node code identities require Clang"
#endif
    }

    template<class T>
    consteval NodeCodeKey make_node_code_key()
    {
        constexpr auto name = clang_type_name<std::remove_cvref_t<T>>();
        return {
            .low = fnv1a64(name, 14695981039346656037ull),
            .high = fnv1a64(name, 1099511628211ull ^ 0x9e3779b97f4a7c15ull),
        };
    }

    template<class T>
    inline constexpr NodeCodeKey node_code_key_v = make_node_code_key<T>();
}
} // namespace iv
