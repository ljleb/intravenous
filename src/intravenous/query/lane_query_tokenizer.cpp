#include "query/lane_query_tokenizer.h"

#include <stdexcept>

namespace iv::query {
namespace {
bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool is_ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || c == '_';
}
} // namespace

std::vector<LaneQueryToken> tokenize_lane_query(std::string_view source)
{
    std::vector<LaneQueryToken> tokens;
    for (size_t i = 0; i < source.size();) {
        if (is_space(source[i])) {
            ++i;
            continue;
        }
        if (source[i] == '.' && i + 1 < source.size() && source[i + 1] == '.') {
            tokens.push_back(LaneQueryToken{.kind = LaneQueryToken::Kind::range, .text = ".."});
            i += 2;
            continue;
        }
        if (source[i] == '-' || is_digit(source[i])) {
            size_t const start = i;
            if (source[i] == '-') {
                ++i;
                if (i >= source.size() || !is_digit(source[i])) {
                    throw std::runtime_error("unexpected '-' in lane query");
                }
            }
            while (i < source.size() && is_digit(source[i])) {
                ++i;
            }
            bool is_float = false;
            if (i < source.size() && source[i] == '.' && (i + 1 >= source.size() || source[i + 1] != '.')) {
                is_float = true;
                ++i;
                if (i >= source.size() || !is_digit(source[i])) {
                    throw std::runtime_error("invalid float literal in lane query");
                }
                while (i < source.size() && is_digit(source[i])) {
                    ++i;
                }
            }
            tokens.push_back(LaneQueryToken{
                .kind = is_float ? LaneQueryToken::Kind::float_number : LaneQueryToken::Kind::int_number,
                .text = std::string(source.substr(start, i - start)),
            });
            continue;
        }
        if (is_ident_char(source[i])) {
            size_t const start = i;
            ++i;
            while (i < source.size() && is_ident_char(source[i])) {
                ++i;
            }
            tokens.push_back(LaneQueryToken{
                .kind = LaneQueryToken::Kind::ident,
                .text = std::string(source.substr(start, i - start)),
            });
            continue;
        }
        switch (source[i]) {
        case '(':
        case ')':
        case '.':
        case ':':
        case '=':
        case '|':
        case '!':
            tokens.push_back(LaneQueryToken{
                .kind = LaneQueryToken::Kind::op,
                .text = std::string(1, source[i]),
            });
            ++i;
            continue;
        default:
            throw std::runtime_error("unexpected character in lane query: " + std::string(1, source[i]));
        }
    }
    tokens.push_back(LaneQueryToken{.kind = LaneQueryToken::Kind::end});
    return tokens;
}
} // namespace iv::query
