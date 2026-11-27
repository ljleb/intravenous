#pragma once

#include <intravenous/graph/error.h>

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>

namespace iv {
namespace details {

constexpr bool valid_port_identifier(std::string_view name)
{
    auto const letter = [](char ch) { return ch >= 'a' && ch <= 'z'; };
    if (name.empty() || !letter(name.front())) return false;
    bool after_underscore = false;
    for (auto ch : name) {
        if (ch == '_' && !after_underscore) {
            after_underscore = true;
        } else if (letter(ch) || (ch >= '0' && ch <= '9')) {
            after_underscore = false;
        } else {
            return false;
        }
    }
    return !after_underscore;
}

constexpr char32_t read_port_name_character(std::string_view& text)
{
    auto const first = static_cast<std::uint8_t>(text.front());
    text.remove_prefix(1);
    if (first < 0x80) return first;
    unsigned const count = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
    if (first < 0xc2 || first > 0xf4 || text.size() < count)
        return 0xff'ffff;
    char32_t character = first & (0x7f >> (count + 1));
    for (unsigned i = 0; i < count; ++i) {
        auto const byte = static_cast<std::uint8_t>(text.front());
        if ((byte & 0xc0) != 0x80) return 0xff'ffff;
        text.remove_prefix(1);
        character = (character << 6) | (byte & 0x3f);
    }
    if ((count == 2 && character < 0x800) || (count == 3 && character < 0x10000)
        || (character >= 0xd800 && character <= 0xdfff))
        return 0xff'ffff;
    return character;
}

constexpr bool port_name_space(char32_t character)
{
    return character == ' ' || character == 0xa0 || character == 0x1680
        || (character >= 0x2000 && character <= 0x200a)
        || character == 0x202f || character == 0x205f || character == 0x3000;
}

constexpr std::string_view port_display_name_error(std::string_view text)
{
    char32_t previous = ' ';
    bool at_start = true;
    std::string_view separator;
    while (!text.empty()) {
        auto const character = read_port_name_character(text);
        if (character > 0x10ffff) return "must be valid UTF-8";
        if (character < ' ' || (character >= 0x7f && character <= 0x9f)
            || character == 0x2028 || character == 0x2029
            || (character >= 0x202a && character <= 0x202e)
            || (character >= 0x2066 && character <= 0x2069))
            return "must be single-line text without control characters";
        if (port_name_space(character)) {
            if (!separator.empty()) return separator;
            if (previous == ' ') return "must be trimmed and use single spaces";
        }
        separator = {};
        if (!at_start && previous == ' ') {
            if (character == '|') separator = "contains reserved separator ' | '";
            if (character == U'·') separator = "contains reserved separator ' · '";
            if (character == U'→') separator = "contains reserved separator ' → '";
        }
        previous = port_name_space(character) ? ' ' : character;
        at_start = false;
    }
    return previous == ' ' ? "must be nonempty and trimmed" : std::string_view{};
}

constexpr void validate_port_names(
    std::string_view identifier, std::string_view display, std::string_view short_display)
{
    if (!valid_port_identifier(identifier))
        error("invalid port identifier '" + std::string(identifier)
            + "'. Use [a-z][a-z0-9]*(?:_[a-z0-9]+)*");
    if (!display.empty()) {
        if (auto issue = port_display_name_error(display); !issue.empty())
            error("port '" + std::string(identifier) + "' display name " + std::string(issue));
    }
    if (!short_display.empty()) {
        if (auto issue = port_display_name_error(short_display); !issue.empty())
            error("port '" + std::string(identifier) + "' short name " + std::string(issue));
    }
}

constexpr std::string copy_port_name(std::string_view name)
{
    std::string result;
    result.reserve(name.size());
    for (char character : name) result.push_back(character);
    return result;
}

template<class Config>
constexpr void validate_port_config_names(Config const& config)
{
    if (!config.name.empty() || !config.display_name.empty() || !config.short_display_name.empty())
        validate_port_names(config.name, config.display_name, config.short_display_name);
}

} // namespace details

struct PortNames {
    template<std::convertible_to<std::string_view> Identifier>
    constexpr PortNames(Identifier const& identifier)
        : PortNames(std::string_view{identifier}, {}) {}

    constexpr PortNames(std::string_view identifier, std::string_view short_name,
        std::string_view long_name = {})
        : identifier(identifier)
        , display(long_name.empty() ? short_name : long_name)
        , short_display(long_name.empty() ? std::string_view{} : short_name)
    {
        // Static port lookup also evaluates the declaration. The builder
        // validates at configuration time so it can report the actual label.
        if !consteval {
            details::validate_port_names(identifier, display, short_display);
        }
        if (short_name.empty() && !long_name.empty())
            details::error("a long port display name requires a short name");
    }

    // A generated port is identified by its position.
    static constexpr PortNames generated() { return PortNames{}; }

    std::string_view identifier;
    std::string_view display;
    std::string_view short_display;

private:
    constexpr PortNames() = default;
};
} // namespace iv
