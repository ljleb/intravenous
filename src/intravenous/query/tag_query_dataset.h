#pragma once

#include <intravenous/query/tag_query_schema.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace iv::query {
class TagQueryDataset {
public:
    virtual ~TagQueryDataset() = default;

    [[nodiscard]] virtual TagQuerySchema const &schema() const = 0;
    [[nodiscard]] virtual size_t entry_count() const = 0;
    [[nodiscard]] virtual bool in_filter(size_t entry_index, std::string_view filter_name) const = 0;
    [[nodiscard]] virtual bool has_unit(size_t entry_index, TagQueryPropertyId property) const = 0;
    [[nodiscard]] virtual std::optional<int> int_value(size_t entry_index, TagQueryPropertyId property) const = 0;
    [[nodiscard]] virtual std::optional<float> float_value(size_t entry_index, TagQueryPropertyId property) const = 0;
};

using TagQueryDatasetPtr = std::shared_ptr<TagQueryDataset const>;
} // namespace iv::query
