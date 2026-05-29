#pragma once

#include "query/lane_query_schema.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace iv {
struct LaneMetadata;
}

namespace iv::query {
class LaneQueryDataset {
public:
    virtual ~LaneQueryDataset() = default;

    [[nodiscard]] virtual LaneQuerySchema const &schema() const = 0;
    [[nodiscard]] virtual size_t lane_count() const = 0;
    [[nodiscard]] virtual std::uint64_t lane_id_at(size_t lane_index) const = 0;
    [[nodiscard]] virtual bool in_filter(size_t lane_index, std::string_view filter_name) const = 0;
    [[nodiscard]] virtual bool has_unit(size_t lane_index, LaneQueryPropertyId property) const = 0;
    [[nodiscard]] virtual std::optional<int> int_value(size_t lane_index, LaneQueryPropertyId property) const = 0;
    [[nodiscard]] virtual std::optional<float> float_value(size_t lane_index, LaneQueryPropertyId property) const = 0;
};

using LaneQueryDatasetPtr = std::shared_ptr<LaneQueryDataset const>;
} // namespace iv::query
