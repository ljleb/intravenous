#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace iv::query {
struct LaneQueryToken {
    enum class Kind : std::uint8_t {
        float_number,
        int_number,
        range,
        ident,
        op,
        end,
    };

    Kind kind = Kind::end;
    std::string text {};
};

[[nodiscard]] std::vector<LaneQueryToken> tokenize_lane_query(std::string_view source);
} // namespace iv::query
