#pragma once

#include "node.h"
#include "random123/aes.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <optional>
#include <random>

namespace iv {
    class UniformNoise {
        std::optional<std::mt19937> _generator;
        std::optional<std::uniform_real_distribution<Sample>> _distribution;
        Sample _min;
        Sample _max;
        std::optional<unsigned int> _seed;

    public:
        constexpr explicit UniformNoise(
            Sample min = -1.0,
            Sample max = 1.0,
            std::optional<unsigned int> seed = {}
        ) :
            _min(min),
            _max(max),
            _seed(seed)
        {}

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickState const& state);
    };

    class DeterministicUniformNoise {
        size_t _seed;

        uint64_t splitmix64(uint64_t index) const;
        double uniform_m11(uint64_t i, Sample min, Sample max) const;

    public:
        constexpr explicit DeterministicUniformNoise(std::optional<Sample> seed = {}) :
            _seed(seed.has_value()
                ? *seed
                : (static_cast<std::uint64_t>(std::random_device{}()) << 32) |
                  static_cast<std::uint64_t>(std::random_device{}()))
        {}

        constexpr auto inputs() const
        {
            return std::array {
                InputConfig { .name = "min", .default_value = -1.0 },
                InputConfig { .name = "max", .default_value = 1.0 },
            };
        }

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickState const& state);
    };

    class DeterministicUniformAESNoise {
        using Rng = r123::AESNI4x32;
        Rng _generator;
        Rng::key_type _seed;

        static Rng::key_type make_seed(std::optional<uint64_t> seed_opt);
        static Rng::ctr_type make_index(uint64_t index);

    public:
        explicit DeterministicUniformAESNoise(std::optional<uint64_t> seed = {});

        constexpr auto inputs() const
        {
            return std::array {
                InputConfig { .name = "min", .default_value = -1.0 },
                InputConfig { .name = "max", .default_value = 1.0 },
            };
        }

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickState const& state);
    };

    class UniformToCauchy {
        Sample _x0;
        Sample _gamma;

    public:
        explicit UniformToCauchy(Sample x0 = 1.0, Sample gamma = 1.0);

        constexpr auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickState const& state);
    };

    class UniformToPower {
        ptrdiff_t _min;
        ptrdiff_t _max;
        Sample _lambda;

        struct State {
            std::span<Sample> weights;
        };

    public:
        explicit UniformToPower(ptrdiff_t min = -5, ptrdiff_t max = 4, Sample lambda = 0.5);

        constexpr auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        template<typename A>
        void init_buffer(A& alloc) const
        {
            State& s = alloc.template new_object<State>();
            size_t range = std::max<ptrdiff_t>(0, _max - _min);
            alloc.assign(s.weights, alloc.template new_array<Sample>(range));
            for (size_t i = 0; i < range; ++i) {
                alloc.assign(alloc.at(s.weights, i), std::powf(_lambda, range - 1 - static_cast<ptrdiff_t>(i)));
            }
            if (alloc.can_allocate()) {
                Sample total = 0.0;
                for (size_t i = 0; i < range; ++i) {
                    total += s.weights[i];
                }
                for (size_t i = 0; i < range; ++i) {
                    s.weights[i] /= total;
                }
            }
        }

        void tick(TickState const& state);
    };

    class UniformToGaussian {
        Sample _mean;
        Sample _std;

    public:
        explicit UniformToGaussian(Sample mean = 0.0, Sample std = 1.0);

        constexpr auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickState const& state);
    };

    class DeterministicGaussianAESNoise {
        using Rng = r123::AESNI4x32;
        Rng _generator;
        Rng::key_type _seed;
        Sample _mean;
        Sample _std;

        static Rng::key_type make_seed(std::optional<uint64_t> seed_opt);
        static Rng::ctr_type make_index(uint64_t index);

    public:
        explicit DeterministicGaussianAESNoise(
            Sample mean = 0.0,
            Sample std = 1.0,
            std::optional<uint64_t> seed = {}
        );

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickState const& state);
    };
}
