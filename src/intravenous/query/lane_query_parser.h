#pragma once

#include <intravenous/query/lane_query_ast.h>

#include <string_view>

namespace iv::query {
class LaneQueryParser {
public:
    [[nodiscard]] RawLaneQueryAst parse(std::string_view source) const;
};
} // namespace iv::query
