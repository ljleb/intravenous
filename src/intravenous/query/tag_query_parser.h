#pragma once

#include <intravenous/query/tag_query_ast.h>

#include <string_view>

namespace iv::query {
class TagQueryParser {
public:
    [[nodiscard]] RawTagQueryAst parse(std::string_view source) const;
};
} // namespace iv::query
