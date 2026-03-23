#pragma once

#include "node.h"
#include <array>

namespace iv {
    struct Warper {
        constexpr auto inputs() const
        {
            return std::array {
                InputConfig { .name = "in" },
                InputConfig { .name = "threshold", .default_value = 1.0 },
            };
        }

        constexpr auto outputs() const
        {
            return std::array {
                OutputConfig { .name = "anti_aliased", .latency = 1 },
                OutputConfig { .name = "aliased" },
            };
        }

        void tick(TickState const& state);
    };

    struct Integrator {
        constexpr auto inputs() const
        {
            return std::array {
                InputConfig { "f_prev" },
                InputConfig { "f" },
                InputConfig { .name = "dt", .default_value = 1.0 },
            };
        }

        constexpr auto outputs() const
        {
            return std::array { OutputConfig { "integral" } };
        }

        void tick(TickState const& state);
    };

    struct Interpolation {
        constexpr auto inputs() const
        {
            return std::array {
                InputConfig { "a" },
                InputConfig { "b" },
                InputConfig { "alpha" },
            };
        }

        constexpr auto outputs() const
        {
            return std::array { OutputConfig { "out" } };
        }

        void tick(TickState const& state);
    };
}
