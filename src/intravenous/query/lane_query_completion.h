#pragma once

#include <intravenous/query/lane_query_schema.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace iv::query {
struct LaneQuerySourceRange {
    size_t start_offset = 0;
    size_t end_offset = 0;
};

enum class LaneQueryCompletionContext {
    property,
    value,
    projection_selector,
    syntax,
};

enum class LaneQueryCompletionKind {
    property,
    unit_literal,
    numeric_literal,
    numeric_range,
    syntax,
};

struct LaneQueryCompletionCandidate {
    LaneQueryCompletionKind kind = LaneQueryCompletionKind::property;
    std::string label {};
    std::string insert_text {};
    std::optional<LaneQueryValueType> value_type {};
};

struct LaneQueryDiagnostic {
    LaneQuerySourceRange range {};
    std::string message {};
};

struct LaneQueryCompletionResult {
    LaneQuerySourceRange replacement_range {};
    LaneQueryCompletionContext context = LaneQueryCompletionContext::property;
    std::vector<LaneQueryCompletionCandidate> candidates {};
    std::vector<LaneQueryDiagnostic> diagnostics {};
};

// `cursor_offset` is a UTF-8 byte offset. Unlike LaneQueryParser this accepts
// incomplete and invalid source because it describes the atom at the cursor,
// rather than attempting to commit an AST.
[[nodiscard]] LaneQueryCompletionResult complete_lane_query(
    std::string_view source,
    size_t cursor_offset,
    LaneQuerySchema const &schema);
} // namespace iv::query
