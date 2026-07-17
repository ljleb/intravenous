#pragma once

#include <cstdint>
#include <cstddef>
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
        invalid,
        end,
    };

    Kind kind = Kind::end;
    std::string text {};
    size_t start_offset = 0;
    size_t end_offset = 0;
};

[[nodiscard]] std::vector<LaneQueryToken> tokenize_lane_query(std::string_view source);
// Editor tooling must remain useful while the user is in the middle of an
// invalid token. This variant retains invalid fragments instead of throwing.
[[nodiscard]] std::vector<LaneQueryToken> tokenize_lane_query_tolerant(std::string_view source);
} // namespace iv::query
