#pragma once
#include "../sample.h"
#include "compat.h"
#include <cmath>


namespace iv {
    enum struct PolyblepSide {
        LEFT,
        RIGHT,
    };

    IV_FORCEINLINE static Sample polyblep_phi(Sample sample, Sample warp_threshold)
    {
        return (sample + warp_threshold) / 2.0;
    }

    IV_FORCEINLINE static Sample polyblep_p(Sample phi, Sample delta, Sample warp_threshold, PolyblepSide side)
    {
        if (side == PolyblepSide::RIGHT && phi < delta)
        {
            Sample first_order = 2.f * phi / delta;
            Sample second_order = phi / delta;
            return (first_order - second_order * second_order - 1) * warp_threshold;
        }
        if (side == PolyblepSide::LEFT && delta > warp_threshold - phi)
        {
            Sample second_order = (phi - warp_threshold) / delta + 1;
            return second_order * second_order * warp_threshold;
        }
        return 0;
    }

    IV_FORCEINLINE static Sample polyblep_error(Sample sample, Sample delta, Sample warp_threshold, PolyblepSide side)
    {
        Sample sign = std::copysignf(1.0, delta);
        delta = std::copysignf(delta, 1.0);

        Sample phi = polyblep_phi(sample, warp_threshold);
        Sample p = polyblep_p(phi, delta, warp_threshold, side) * sign;
        return p;
    }

    IV_FORCEINLINE static Sample warp_pm1(Sample x, Sample limit)
    {
        Sample period = Sample(2.0 * limit);
        return x - std::floor((x + limit) / period) * period;
    }
}
