#pragma once

#include "query/lane_query_dataset.h"
#include "query/lane_query_program.h"

#include <cstddef>
#include <vector>

namespace iv::query {
struct LaneQueryExecutionResult {
    std::vector<size_t> matching_lane_indexes {};
};

[[nodiscard]] LaneQueryExecutionResult execute_lane_query(
    BoundLaneQueryAst const &query,
    LaneQueryDataset const &dataset);
} // namespace iv::query
