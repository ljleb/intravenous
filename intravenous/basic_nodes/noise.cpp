#include "noise.h"

#include "math/erfinv.h"
#include "random123/uniform.hpp"

#include <bit>
#include <numbers>

namespace iv {
    void UniformNoise::tick(TickState const& state)
    {
        if (!_generator.has_value()) {
            _generator.emplace(_seed.has_value() ? _seed.value() : std::random_device{}());
            _distribution.emplace(_min, _max);
        }
        state.outputs[0].push((*_distribution)(*_generator));
    }

    uint64_t DeterministicUniformNoise::splitmix64(uint64_t index) const
    {
        size_t z = _seed + index * 0x9e3779b97f4a7c15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    double DeterministicUniformNoise::uniform_m11(uint64_t i, Sample min, Sample max) const
    {
        uint64_t mantissa = i >> (64 - 52);
        uint64_t bits = 0x4000000000000000ULL | mantissa;
        double range = (max - min) / 2.0;
        double min_reinterpret = 2.0 * min - max;
        return std::bit_cast<double>(bits) * range + min_reinterpret;
    }

    void DeterministicUniformNoise::tick(TickState const& state)
    {
        auto const min = state.inputs[0].get();
        auto const max = state.inputs[1].get();
        uint64_t uniform_int = splitmix64(state.index);
        state.outputs[0].push(uniform_m11(uniform_int, min, max));
    }

    DeterministicUniformAESNoise::Rng::key_type DeterministicUniformAESNoise::make_seed(std::optional<uint64_t> seed_opt)
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

    DeterministicUniformAESNoise::Rng::ctr_type DeterministicUniformAESNoise::make_index(uint64_t index)
    {
        return {
            static_cast<uint32_t>(index),
            static_cast<uint32_t>(index >> 32),
            0,
            0,
        };
    }

    DeterministicUniformAESNoise::DeterministicUniformAESNoise(std::optional<uint64_t> seed) :
        _seed(make_seed(seed))
    {
        assert(haveAESNI() && "This machine does not have the AES-NI instruction set, use a different noise node.");
    }

    void DeterministicUniformAESNoise::tick(TickState const& state)
    {
        auto const min = state.inputs[0].get();
        auto const max = state.inputs[1].get();
        Rng::ctr_type counter = make_index(state.index);
        unsigned int uniform_uint = _generator(counter, _seed)[0];
        state.outputs[0].push(r123::u01<Sample>(uniform_uint) * (max - min) + min);
    }

    UniformToCauchy::UniformToCauchy(Sample x0, Sample gamma) :
        _x0(x0),
        _gamma(gamma)
    {
    }

    void UniformToCauchy::tick(TickState const& state)
    {
        Sample uniform = state.inputs[0].get();
        state.outputs[0].push(_x0 + _gamma * std::tanf(std::numbers::pi_v<float> * uniform * 0.5));
    }

    UniformToPower::UniformToPower(ptrdiff_t min, ptrdiff_t max, Sample lambda) :
        _min(min),
        _max(max),
        _lambda(lambda)
    {
    }

    void UniformToPower::tick(TickState const& state)
    {
        auto const& s = state.get_state<State>();
        Sample uniform = state.inputs[0].get() * 0.5 + 0.5;
        size_t discrete = static_cast<size_t>(
            std::lower_bound(s.weights.begin(), s.weights.end(), uniform) - s.weights.begin()
        );
        state.outputs[0].push(std::exp2f(static_cast<Sample>(discrete)));
    }

    UniformToGaussian::UniformToGaussian(Sample mean, Sample std) :
        _mean(mean),
        _std(std)
    {
    }

    void UniformToGaussian::tick(TickState const& state)
    {
        Sample uniform = state.inputs[0].get();
        Sample normal = std::numbers::sqrt2_v<float> * erfinvf(uniform);
        state.outputs[0].push(std::fmaf(normal, _std, _mean));
    }

    DeterministicGaussianAESNoise::Rng::key_type DeterministicGaussianAESNoise::make_seed(std::optional<uint64_t> seed_opt)
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

    DeterministicGaussianAESNoise::Rng::ctr_type DeterministicGaussianAESNoise::make_index(uint64_t index)
    {
        return {
            static_cast<uint32_t>(index),
            static_cast<uint32_t>(index >> 32),
            0,
            0,
        };
    }

    DeterministicGaussianAESNoise::DeterministicGaussianAESNoise(
        Sample mean,
        Sample std,
        std::optional<uint64_t> seed
    ) :
        _seed(make_seed(seed)),
        _mean(mean),
        _std(std)
    {
        assert(haveAESNI() && "This machine does not have the AES-NI instruction set, use a different noise node.");
    }

    void DeterministicGaussianAESNoise::tick(TickState const& state)
    {
        Rng::ctr_type counter = make_index(state.index);
        unsigned int uniform_uint = _generator(counter, _seed)[0];
        Sample uniform = r123::uneg11<Sample>(uniform_uint);
        Sample gaussian = std::numbers::sqrt2_v<float> * erfinvf(uniform);
        state.outputs[0].push(std::fmaf(gaussian, _std, _mean));
    }
}
