#pragma once

#include "node_lifecycle.h"

#include "math/erfinv.h"
#include "random123/aes.h"
#include "random123/uniform.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <numbers>

namespace iv {
    namespace details {
        inline Sample clamp_open_unit_interval_pm1(Sample x)
        {
            float const lower = std::nextafter(-1.0f, 0.0f);
            float const upper = std::nextafter(1.0f, 0.0f);
            return Sample(std::clamp(static_cast<float>(x), lower, upper));
        }
    }

    class UniformNoise {
        Sample _min;
        Sample _max;
        unsigned int _seed;

    public:
        explicit UniformNoise(
            Sample min = -1.0,
            Sample max = 1.0,
            std::optional<unsigned int> seed = {}
        )
        : _min(min)
        , _max(max)
        , _seed(seed.has_value() ? seed.value() : std::random_device{}())
        {}

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        struct State {
            std::mt19937 generator;
            std::uniform_real_distribution<Sample::storage> distribution;
        };

        void initialize(InitializationContext<UniformNoise> const& ctx) const
        {
            auto& state = ctx.state();
            state.generator.seed(_seed);
            state.distribution = std::uniform_real_distribution<Sample::storage>(_min, _max);
        }

        void tick(TickSampleContext<UniformNoise> const& ctx) const
        {
            auto& state = ctx.state();
            ctx.outputs[0].push((state.distribution)(state.generator));
        }
    };

    class DeterministicUniformNoise {
        size_t _seed;

        uint64_t splitmix64(uint64_t index) const
        {
            size_t z = _seed + index * 0x9e3779b97f4a7c15ULL;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            return z ^ (z >> 31);
        }

        double uniform_m11(uint64_t i, Sample min, Sample max) const
        {
            uint64_t mantissa = i >> (64 - 52);
            uint64_t bits = 0x4000000000000000ULL | mantissa;
            double range = (max - min) / 2.0;
            double min_reinterpret = 2.0 * min - max;
            return std::bit_cast<double>(bits) * range + min_reinterpret;
        }

    public:
        explicit DeterministicUniformNoise(std::optional<Sample> seed = {}) :
            _seed(seed.has_value()
                ? *seed
                : static_cast<Sample>((static_cast<std::uint64_t>(std::random_device{}()) << 32) |
                  static_cast<std::uint64_t>(std::random_device{}())))
        {}

        auto inputs() const
        {
            return std::array {
                InputConfig { .name = "min", .default_value = -1.0 },
                InputConfig { .name = "max", .default_value = 1.0 },
            };
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickSampleContext<DeterministicUniformNoise> const& state) const
        {
            auto const min = state.inputs[0].get();
            auto const max = state.inputs[1].get();
            uint64_t uniform_int = splitmix64(state.index);
            state.outputs[0].push(uniform_m11(uniform_int, min, max));
        }
    };

    class DeterministicUniformAESNoise {
        using Rng = r123::AESNI4x32;
        Rng _generator;
        Rng::key_type _seed;

        static Rng::key_type make_seed(std::optional<uint64_t> seed_opt)
        {
            uint32_t seed_low;
            uint32_t seed_high;
            if (seed_opt.has_value()) {
                uint64_t seed = *seed_opt;
                seed_low = static_cast<uint32_t>(seed);
                seed_high = static_cast<uint32_t>(seed >> 32);
            } else {
                seed_low = std::random_device{}();
                seed_high = std::random_device{}();
            }
            return Rng::ukey_type { seed_low, seed_high, 0, 0 };
        }

        static Rng::ctr_type make_index(uint64_t index)
        {
            return {
                static_cast<uint32_t>(index),
                static_cast<uint32_t>(index >> 32),
                0,
                0,
            };
        }

    public:
        explicit DeterministicUniformAESNoise(std::optional<uint64_t> seed = {})
        : _seed(make_seed(seed))
        {
            IV_ASSERT(haveAESNI(), "This machine does not have the AES-NI instruction set, use a different noise node.");
        }

        auto inputs() const
        {
            return std::array {
                InputConfig { .name = "min", .default_value = -1.0 },
                InputConfig { .name = "max", .default_value = 1.0 },
            };
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick_block(TickBlockContext<DeterministicUniformAESNoise> const& ctx) const
        {
            auto const min = ctx.inputs[0].get();
            auto const max = ctx.inputs[1].get();
            auto const scale = max - min;

            auto const start = ctx.index;
            auto const end   = start + ctx.block_size;

            auto const first_group = start >> 2;
            auto const last_group  = (end - 1) >> 2;

            auto const to_sample = [&](uint32_t x) -> Sample {
                return r123::u01<Sample>(x) * scale + min;
            };

            for (uint64_t group = first_group; group <= last_group; ++group) {
                auto const r = _generator(make_index(group), _seed);

                auto const tmp = std::to_array({
                    to_sample(r[0]),
                    to_sample(r[1]),
                    to_sample(r[2]),
                    to_sample(r[3]),
                });

                auto const first_lane = (group == first_group) ? (start & 3u) : 0u;
                auto const last_lane  = (group == last_group)  ? ((end - 1) & 3u) : 3u;

                std::span<Sample const> span{tmp.data() + first_lane, last_lane - first_lane + 1};
                ctx.outputs[0].push_block(span);
            }
        }
    };

    class UniformToCauchy {
        Sample _x0;
        Sample _gamma;

    public:
        explicit UniformToCauchy(Sample x0 = 1.0, Sample gamma = 1.0)
        : _x0(x0)
        , _gamma(gamma)
        {}

        auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickSampleContext<UniformToCauchy> const& state) const
        {
            Sample uniform = details::clamp_open_unit_interval_pm1(state.inputs[0].get());
            state.outputs[0].push(_x0 + _gamma * std::tanf(std::numbers::pi_v<float> * uniform * 0.5));
        }
    };

    class UniformToPower {
        ptrdiff_t _min;
        ptrdiff_t _max;
        Sample _lambda;

    public:
        struct State {
            std::span<Sample> weights;
            ptrdiff_t min;
            ptrdiff_t sign;
        };

        explicit UniformToPower(ptrdiff_t min = -5, ptrdiff_t max = 4, Sample lambda = 0.5)
        : _min(min)
        , _max(max)
        , _lambda(lambda)
        {}

        auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void declare(DeclarationContext<UniformToPower> const& ctx) const
        {
            auto const& state = ctx.state();

            size_t range = static_cast<size_t>(std::abs(_max - _min)) + 1;
            ctx.local_array(state.weights, range);
        }

        void initialize(InitializationContext<UniformToPower> const& ctx) const
        {
            auto& state = ctx.state();

            for (size_t i = 0; i < state.weights.size(); ++i) {
                state.weights[i] = std::powf(_lambda, state.weights.size() - 1 - i);
            }
            Sample total = 0.0;
            for (size_t i = 0; i < state.weights.size(); ++i) {
                total += state.weights[i];
            }
            for (size_t i = 0; i < state.weights.size(); ++i) {
                state.weights[i] /= total;
            }
            Sample cumsum = 0;
            for (size_t i = 0; i < state.weights.size(); ++i) {
                cumsum += state.weights[i];
                state.weights[i] = cumsum;
            }

            state.min = _min;
            state.sign = (_max >= _min) ? 1 : -1;
        }

        void tick(TickSampleContext<UniformToPower> const& ctx) const
        {
            auto& state = ctx.state();
            auto const uniform = std::clamp<Sample>(
                ctx.inputs[0].get() * Sample{0.5} + Sample{0.5},
                Sample{0},
                std::nextafter(Sample{1}, Sample{0})
            );
            auto const discrete = static_cast<size_t>(
                std::lower_bound(state.weights.begin(), state.weights.end(), uniform) - state.weights.begin()
            );
            auto const exponent = state.min + static_cast<ptrdiff_t>(discrete) * state.sign;
            ctx.outputs[0].push(std::exp2f(static_cast<Sample>(exponent)));
        }
    };

    class UniformToGaussian {
        Sample _mean;
        Sample _std;

    public:
        explicit UniformToGaussian(Sample mean = 0.0, Sample std = 1.0)
        : _mean(mean)
        , _std(std)
        {}

        auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickSampleContext<UniformToGaussian> const& state) const
        {
            Sample uniform = details::clamp_open_unit_interval_pm1(state.inputs[0].get());
            Sample normal = std::numbers::sqrt2_v<float> * erfinvf(uniform);
            state.outputs[0].push(std::fmaf(normal, _std, _mean));
        }
    };

    class DeterministicGaussianAESNoise {
        using Rng = r123::AESNI4x32;
        Rng _generator;
        Rng::key_type _seed;
        Sample _mean;
        Sample _std;

        static Rng::key_type make_seed(std::optional<uint64_t> seed_opt)
        {
            uint32_t seed_low;
            uint32_t seed_high;
            if (seed_opt.has_value()) {
                uint64_t seed = *seed_opt;
                seed_low = static_cast<uint32_t>(seed);
                seed_high = static_cast<uint32_t>(seed >> 32);
            } else {
                seed_low = std::random_device{}();
                seed_high = std::random_device{}();
            }
            return Rng::ukey_type { seed_low, seed_high, 0, 0 };
        }

        static Rng::ctr_type make_index(uint64_t index)
        {
            return {
                static_cast<uint32_t>(index),
                static_cast<uint32_t>(index >> 32),
                0,
                0,
            };
        }

    public:
        explicit DeterministicGaussianAESNoise(
            Sample mean = 0.0,
            Sample std = 1.0,
            std::optional<uint64_t> seed = {}
        )
        : _seed(make_seed(seed))
        , _mean(mean)
        , _std(std)
        {
            IV_ASSERT(haveAESNI(), "This machine does not have the AES-NI instruction set, use a different noise node.");
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickSampleContext<DeterministicGaussianAESNoise> const& state) const
        {
            Rng::ctr_type counter = make_index(state.index);
            unsigned int uniform_uint = _generator(counter, _seed)[0];
            Sample uniform = details::clamp_open_unit_interval_pm1(r123::uneg11<Sample>(uniform_uint));
            Sample gaussian = std::numbers::sqrt2_v<float> * erfinvf(uniform);
            state.outputs[0].push(std::fmaf(gaussian, _std, _mean));
        }
    };
}
