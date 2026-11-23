#pragma once

namespace iv::automation {

// Weighted harmonic endpoint derivative for monotone cubic interpolation.
inline double curve_tangent(double previous, double next, double before, double after)
{
    if (!previous || !next || (previous < 0) != (next < 0)) return 0;
    auto const a = 2 * after + before, b = after + 2 * before;
    return (a + b) / (a / previous + b / next);
}

} // namespace iv::automation
