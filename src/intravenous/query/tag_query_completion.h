#pragma once

#include <intravenous/query/tag_query_schema.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace iv::query {
struct TagQuerySourceRange {
    size_t start_offset = 0;
    size_t end_offset = 0;
};

enum class TagQueryCompletionContext {
    property,
    value,
    projection_selector,
    syntax,
};

enum class TagQueryCompletionKind {
    property,
    unit_literal,
    numeric_literal,
    numeric_range,
    syntax,
};

struct TagQueryCompletionCandidate {
    TagQueryCompletionKind kind = TagQueryCompletionKind::property;
    std::string label {};
    std::string insert_text {};
    std::optional<TagQueryValueType> value_type {};
};

struct TagQueryDiagnostic {
    TagQuerySourceRange range {};
    std::string message {};
};

struct TagQueryCompletionResult {
    TagQuerySourceRange replacement_range {};
    TagQueryCompletionContext context = TagQueryCompletionContext::property;
    std::vector<TagQueryCompletionCandidate> candidates {};
    std::vector<TagQueryDiagnostic> diagnostics {};
};

// `cursor_offset` is a UTF-8 byte offset. Unlike TagQueryParser this accepts
// incomplete and invalid source because it describes the atom at the cursor,
// rather than attempting to commit an AST.
[[nodiscard]] TagQueryCompletionResult complete_tag_query(
    std::string_view source,
    size_t cursor_offset,
    TagQuerySchema const &schema);
} // namespace iv::query
