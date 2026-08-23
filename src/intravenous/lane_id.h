#pragma once

#include <cstdint>

namespace iv {
struct LaneId {
    std::uint64_t value = 0;

    constexpr explicit operator bool() const noexcept
    {
        return value != 0;
    }

    bool operator==(LaneId const&) const = default;
};
} // namespace iv
