#pragma once

#include "node.h"
#include <array>

namespace iv {
    struct WhackIirThing {
        auto inputs() const
        {
            return std::array<InputConfig, 3>{};
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 2>{};
        }

        void tick(TickState const& state) const;
    };

    struct SimpleIirHighPass {
        static constexpr Sample FMIN = 2e1f;
        static constexpr Sample FMAX = 2e4f;

        auto inputs() const
        {
            return std::array {
                InputConfig { .name = "in", .history = 1 },
                InputConfig { .name = "cutoff" },
                InputConfig { .name = "dx", .default_value = 1.0 },
            };
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickState const& state) const;
    };

    struct SimpleIirLowPass {
        static constexpr Sample FMIN = 2e1f;
        static constexpr Sample FMAX = 2e4f;

        auto inputs() const
        {
            return std::array {
                InputConfig { .name = "in", .history = 1 },
                InputConfig { .name = "cutoff" },
                InputConfig { .name = "dx", .default_value = 1.0 },
            };
        }

        auto outputs() const
        {
            return std::array { OutputConfig { "out" } };
        }

        void tick(TickState const& state) const;
    };
}
