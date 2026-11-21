#pragma once

#include <array>
#include <concepts>
#include <string_view>

namespace iv {
struct Tag {
    Tag const* parent;
    std::string_view segment;

    consteval explicit Tag(std::string_view name) : Tag(nullptr, name) {
        if (name == "id" || name == "filter") throw "this tag root is reserved by the query language";
    }

    consteval Tag(Tag const& parent, std::string_view name) : Tag(&parent, name) {}

private:
    consteval Tag(Tag const* parent, std::string_view name) : parent(parent), segment(name) {
        auto const starts_identifier = [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; };
        if (name.empty() || !starts_identifier(name.front()) || name == "unit")
            throw "a tag segment must be a query identifier";
        for (auto c : name.substr(1)) {
            if (!starts_identifier(c) && !(c >= '0' && c <= '9') && c != '-')
                throw "a tag segment must be a query identifier";
        }
    }
};

template<class... Tags>
requires (std::same_as<Tags, Tag> && ...)
[[nodiscard]] consteval auto tags(Tags const&... values) {
    return std::array<Tag const*, sizeof...(Tags)>{&values...};
}

namespace tag {
inline constexpr Tag automation{"automation"};
inline constexpr Tag curve{automation, "curve"};
inline constexpr Tag rhythm{"rhythm"};
inline constexpr Tag beats{rhythm, "beats"};
inline constexpr Tag timeline{"timeline"};
inline constexpr Tag device{"device"};
inline constexpr Tag input{device, "input"};
inline constexpr Tag output{device, "output"};
inline constexpr Tag oscillator{"oscillator"};
}
}
