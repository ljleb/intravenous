#include <intravenous/query/tag_query_completion.h>

#include <intravenous/query/tag_query_tokenizer.h>

#include <algorithm>
#include <string_view>
#include <utility>

namespace iv::query {
namespace {
bool is_atom_token(TagQueryToken const &token)
{
    return token.kind == TagQueryToken::Kind::ident
        || token.kind == TagQueryToken::Kind::int_number
        || token.kind == TagQueryToken::Kind::float_number;
}

size_t previous_token_index(std::vector<TagQueryToken> const &tokens, size_t offset)
{
    size_t result = tokens.size();
    for (size_t index = 0; index < tokens.size(); ++index) {
        auto const &token = tokens[index];
        if (token.kind == TagQueryToken::Kind::end || token.end_offset > offset) {
            break;
        }
        result = index;
    }
    return result;
}

size_t token_at_cursor(std::vector<TagQueryToken> const &tokens, size_t cursor)
{
    for (size_t index = 0; index < tokens.size(); ++index) {
        auto const &token = tokens[index];
        if (token.kind == TagQueryToken::Kind::end) {
            break;
        }
        if (token.start_offset <= cursor && cursor < token.end_offset) {
            return index;
        }
        // At the end of an atom, keep editing that atom unless another token
        // begins at the same byte (for example, the '=' in `gain=`).
        if (cursor == token.end_offset
            && (index + 1 == tokens.size()
                || tokens[index + 1].kind == TagQueryToken::Kind::end
                || tokens[index + 1].start_offset > cursor)) {
            return index;
        }
    }
    return tokens.size();
}

bool preceding_token_is(
    std::vector<TagQueryToken> const &tokens,
    size_t token_index,
    std::string_view text)
{
    return token_index != 0 && token_index <= tokens.size()
        && tokens[token_index - 1].text == text;
}

std::string path_prefix_before(
    std::vector<TagQueryToken> const &tokens,
    size_t replacement_start)
{
    auto index = previous_token_index(tokens, replacement_start);
    if (index == tokens.size() || tokens[index].text != ".") {
        return {};
    }

    std::vector<std::string> parts;
    while (index != tokens.size() && tokens[index].text == ".") {
        if (index == 0 || tokens[index - 1].kind != TagQueryToken::Kind::ident) {
            break;
        }
        parts.push_back(tokens[index - 1].text);
        if (index < 2) {
            break;
        }
        index -= 2;
    }
    std::reverse(parts.begin(), parts.end());
    std::string prefix;
    for (auto const &part : parts) {
        if (!prefix.empty()) {
            prefix += '.';
        }
        prefix += part;
    }
    if (!prefix.empty()) {
        prefix += '.';
    }
    return prefix;
}

std::optional<TagQueryValueType> assigned_property_type(
    std::vector<TagQueryToken> const &tokens,
    size_t cursor,
    TagQuerySchema const &schema)
{
    auto equals = previous_token_index(tokens, cursor);
    while (equals != tokens.size() && tokens[equals].text != "=") {
        if (tokens[equals].text == "|" || tokens[equals].text == "(") {
            return std::nullopt;
        }
        if (equals == 0) {
            return std::nullopt;
        }
        --equals;
    }
    if (equals == tokens.size() || tokens[equals].text != "=" || equals == 0) {
        return std::nullopt;
    }

    std::vector<std::string> parts;
    auto index = equals - 1;
    while (true) {
        if (tokens[index].kind != TagQueryToken::Kind::ident) {
            return std::nullopt;
        }
        parts.push_back(tokens[index].text);
        if (index < 2 || tokens[index - 1].text != ".") {
            break;
        }
        index -= 2;
    }
    std::reverse(parts.begin(), parts.end());
    std::string key;
    for (auto const &part : parts) {
        if (!key.empty()) {
            key += '.';
        }
        key += part;
    }
    auto const id = schema.find(key);
    return id.has_value() ? std::optional<TagQueryValueType>(schema.type_of(*id)) : std::nullopt;
}

void append_property_candidates(
    TagQueryCompletionResult &result,
    std::string_view typed,
    std::string const &path_prefix,
    TagQuerySchema const &schema)
{
    for (auto const &entry : schema.entries()) {
        if (!entry.key.starts_with(path_prefix)) {
            continue;
        }
        auto const suffix = std::string_view(entry.key).substr(path_prefix.size());
        if (!suffix.starts_with(typed) || suffix.empty()) {
            continue;
        }
        result.candidates.push_back(TagQueryCompletionCandidate{
            .kind = TagQueryCompletionKind::property,
            .label = entry.key,
            .insert_text = std::string(suffix),
            .value_type = entry.type,
        });
    }
}

void append_value_candidates(
    TagQueryCompletionResult &result,
    std::optional<TagQueryValueType> type)
{
    if (!type.has_value()) {
        return;
    }
    if (*type == TagQueryValueType::unit) {
        result.candidates.push_back(TagQueryCompletionCandidate{
            .kind = TagQueryCompletionKind::unit_literal,
            .label = "unit",
            .insert_text = "unit",
            .value_type = type,
        });
        return;
    }
    result.candidates.push_back(TagQueryCompletionCandidate{
        .kind = TagQueryCompletionKind::numeric_literal,
        .label = "0",
        .insert_text = "0",
        .value_type = type,
    });
    result.candidates.push_back(TagQueryCompletionCandidate{
        .kind = TagQueryCompletionKind::numeric_range,
        .label = "0..1",
        .insert_text = "0..1",
        .value_type = type,
    });
}

void append_syntax_candidate(TagQueryCompletionResult &result, std::string text)
{
    result.candidates.push_back(TagQueryCompletionCandidate{
        .kind = TagQueryCompletionKind::syntax,
        .label = text,
        .insert_text = std::move(text),
    });
}
} // namespace

TagQueryCompletionResult complete_tag_query(
    std::string_view source,
    size_t cursor_offset,
    TagQuerySchema const &schema)
{
    auto const cursor = std::min(cursor_offset, source.size());
    auto const tokens = tokenize_tag_query_tolerant(source);
    auto const current = token_at_cursor(tokens, cursor);
    TagQuerySourceRange range{.start_offset = cursor, .end_offset = cursor};
    std::string_view typed;
    if (current != tokens.size() && is_atom_token(tokens[current])) {
        range = TagQuerySourceRange{
            .start_offset = tokens[current].start_offset,
            .end_offset = tokens[current].end_offset,
        };
        typed = tokens[current].text;
    }

    TagQueryCompletionResult result{.replacement_range = range};
    for (auto const &token : tokens) {
        if (token.kind == TagQueryToken::Kind::invalid) {
            result.diagnostics.push_back(TagQueryDiagnostic{
                .range = TagQuerySourceRange{
                    .start_offset = token.start_offset,
                    .end_offset = token.end_offset,
                },
                .message = "invalid tag query token: '" + token.text + "'",
            });
        }
    }
    auto const prior = previous_token_index(tokens, range.start_offset);
    auto const selector = current != tokens.size()
        ? preceding_token_is(tokens, current, ":")
        : prior != tokens.size() && tokens[prior].text == ":";
    if (selector) {
        result.context = TagQueryCompletionContext::projection_selector;
        append_property_candidates(result, typed, path_prefix_before(tokens, range.start_offset), schema);
        return result;
    }

    auto const has_assignment = assigned_property_type(tokens, range.start_offset, schema);
    auto const after_assignment = (current != tokens.size() && preceding_token_is(tokens, current, "="))
        || (prior != tokens.size() && tokens[prior].text == "=");
    if (after_assignment || has_assignment.has_value()) {
        result.context = TagQueryCompletionContext::value;
        append_value_candidates(result, has_assignment);
        return result;
    }

    auto const previous_is_expression = prior != tokens.size()
        && (is_atom_token(tokens[prior]) || tokens[prior].text == ")");
    if (current == tokens.size() && previous_is_expression
        && range.start_offset == cursor && !source.empty()
        && (cursor == 0 || source[cursor - 1] != '.')) {
        result.context = TagQueryCompletionContext::syntax;
        append_syntax_candidate(result, "=");
        append_syntax_candidate(result, ".");
        append_syntax_candidate(result, "|");
        auto open_groups = 0;
        for (auto const &token : tokens) {
            if (token.end_offset > cursor) break;
            if (token.text == "(") ++open_groups;
            if (token.text == ")") --open_groups;
        }
        if (open_groups > 0) {
            append_syntax_candidate(result, ")");
        }
        return result;
    }

    result.context = TagQueryCompletionContext::property;
    append_property_candidates(result, typed, path_prefix_before(tokens, range.start_offset), schema);
    return result;
}
} // namespace iv::query
