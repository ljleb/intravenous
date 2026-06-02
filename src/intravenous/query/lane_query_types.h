#pragma once

#include <cstdint>

namespace iv::query {
enum class LaneQueryExprType : std::uint8_t {
    element_set,
    unit_value_set,
    int_value_set,
    float_value_set,
};
} // namespace iv::query
