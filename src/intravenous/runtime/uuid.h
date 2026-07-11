#pragma once

#include <compare>
#include <functional>
#include <string>
#include <string_view>

namespace iv {
class InternedString {
    char const *value_ = "";

    explicit InternedString(char const *value)
        : value_(value)
    {
    }

public:
    InternedString() = default;

    [[nodiscard]] static InternedString from_string(std::string value);
    [[nodiscard]] static InternedString from_view(std::string_view value);

    [[nodiscard]] bool empty() const;
    [[nodiscard]] std::string_view view() const;
    [[nodiscard]] std::string str() const;
    [[nodiscard]] char const *c_str() const;

    auto operator<=>(InternedString const &other) const
    {
        return view() <=> other.view();
    }

    bool operator==(InternedString const &other) const
    {
        return value_ == other.value_;
    }
};

[[nodiscard]] InternedString generate_uuid_v4();
} // namespace iv

template<>
struct std::hash<iv::InternedString> {
    size_t operator()(iv::InternedString const &value) const noexcept
    {
        return std::hash<char const *>{}(value.c_str());
    }
};
