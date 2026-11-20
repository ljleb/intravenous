#include <intravenous/query/tag_query_tokenizer.h>

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

std::vector<TagQueryToken> tokenize_impl(std::string_view source, bool tolerant)
{
    std::vector<TagQueryToken> tokens;
    for (size_t i = 0; i < source.size();) {
        if (is_space(source[i])) {
            ++i;
            continue;
        }
        if (source[i] == '.' && i + 1 < source.size() && source[i + 1] == '.') {
            tokens.push_back(TagQueryToken{
                .kind = TagQueryToken::Kind::range,
                .text = "..",
                .start_offset = i,
                .end_offset = i + 2,
            });
            i += 2;
            continue;
        }
        if (source[i] == '-' || is_digit(source[i])) {
            size_t const start = i;
            if (source[i] == '-') {
                ++i;
                if (i >= source.size() || !is_digit(source[i])) {
                    if (!tolerant) {
                        throw std::runtime_error("unexpected '-' in tag query");
                    }
                    tokens.push_back(TagQueryToken{
                        .kind = TagQueryToken::Kind::invalid,
                        .text = "-",
                        .start_offset = start,
                        .end_offset = i,
                    });
                    continue;
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
                    if (!tolerant) {
                        throw std::runtime_error("invalid float literal in tag query");
                    }
                }
                while (i < source.size() && is_digit(source[i])) {
                    ++i;
                }
            }
            tokens.push_back(TagQueryToken{
                .kind = is_float ? TagQueryToken::Kind::float_number : TagQueryToken::Kind::int_number,
                .text = std::string(source.substr(start, i - start)),
                .start_offset = start,
                .end_offset = i,
            });
            continue;
        }
        if (is_ident_char(source[i])) {
            size_t const start = i;
            ++i;
            while (i < source.size() && (is_ident_char(source[i]) || is_digit(source[i]) || source[i] == '-')) {
                ++i;
            }
            tokens.push_back(TagQueryToken{
                .kind = TagQueryToken::Kind::ident,
                .text = std::string(source.substr(start, i - start)),
                .start_offset = start,
                .end_offset = i,
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
            tokens.push_back(TagQueryToken{
                .kind = TagQueryToken::Kind::op,
                .text = std::string(1, source[i]),
                .start_offset = i,
                .end_offset = i + 1,
            });
            ++i;
            continue;
        default:
            if (!tolerant) {
                throw std::runtime_error("unexpected character in tag query: " + std::string(1, source[i]));
            }
            tokens.push_back(TagQueryToken{
                .kind = TagQueryToken::Kind::invalid,
                .text = std::string(1, source[i]),
                .start_offset = i,
                .end_offset = i + 1,
            });
            ++i;
            continue;
        }
    }
    tokens.push_back(TagQueryToken{
        .kind = TagQueryToken::Kind::end,
        .start_offset = source.size(),
        .end_offset = source.size(),
    });
    return tokens;
}

std::vector<TagQueryToken> tokenize_tag_query(std::string_view source)
{
    return tokenize_impl(source, false);
}

std::vector<TagQueryToken> tokenize_tag_query_tolerant(std::string_view source)
{
    return tokenize_impl(source, true);
}
} // namespace iv::query
