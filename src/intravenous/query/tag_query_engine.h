#pragma once

#include <intravenous/query/tag_query_dataset.h>
#include <intravenous/query/tag_query_program.h>

#include <cstddef>
#include <vector>

namespace iv::query {
struct TagQueryExecutionResult {
    std::vector<size_t> matching_entry_indexes {};
};

[[nodiscard]] TagQueryExecutionResult execute_tag_query(
    BoundTagQueryAst const &query,
    TagQueryDataset const &dataset);
} // namespace iv::query
