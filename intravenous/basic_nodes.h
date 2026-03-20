#pragma once
#include "node.h"
#include "alligator.h"
#include "polyblep.h"
#include "fast_bitset.h"
#include "note_number_lookup_table.h"
#include "random123/aes.h"
#include "random123/uniform.hpp"
#include "math/erfinv.h"
#include <functional>
#include <array>
#include <optional>
#include <random>
#include <vector>
#include <memory>
#include <cstddef>
#include <cstring>


namespace iv {
    template<typename BinaryOp>
    constexpr Sample binary_op_default_v = 0.0;

    template<typename T>
    constexpr T binary_op_default_v<std::multiplies<T>> = T(1.0);

    template<typename T>
    constexpr T binary_op_default_v<std::divides<T>> = T(1.0);

    template<typename BinaryOp>
    class BinaryOpNode {
        BinaryOp _binary_op;
        size_t _num_inputs;

    public:
        constexpr explicit BinaryOpNode(size_t num_inputs = 2) :
            _num_inputs(num_inputs)
        {}

        constexpr auto inputs() const
        {
            return std::vector<InputConfig>(_num_inputs);
        }

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        constexpr auto num_inputs() const
        {
            return _num_inputs;
        }

        constexpr void tick(TickState const& state)
        {
            auto& out = state.outputs[0];
            Sample result = binary_op_default_v<BinaryOp>;
            for (auto& input : state.inputs)
            {
                result = _binary_op(result, input.get());
            }
            out.push(result);
        }
    };

    using Sum = BinaryOpNode<std::plus<Sample>>;
    using Subtract = BinaryOpNode<std::minus<Sample>>;
    using Product = BinaryOpNode<std::multiplies<Sample>>;
    using Quotient = BinaryOpNode<std::divides<Sample>>;
    
    struct Invert {
        constexpr explicit Invert()
        {}

        constexpr auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickState const& state)
        {
            state.outputs[0].push(-state.inputs[0].get());
        }
    };

    struct Power {
        constexpr explicit Power()
        {}

        constexpr auto inputs() const
        {
            return std::array<InputConfig, 2>{};
        }

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickState const& state)
        {
            Sample result = std::powf(state.inputs[0].get(), state.inputs[1].get());
            state.outputs[0].push(result);
        }
    };

    class Broadcast {
        size_t _num_outputs;

    public:
        constexpr explicit Broadcast(size_t num_outputs) :
            _num_outputs(num_outputs)
        {}

        constexpr auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        constexpr auto outputs() const
        {
            return std::vector<OutputConfig>(_num_outputs);
        }

        constexpr auto num_outputs() const
        {
            return _num_outputs;
        }

        void tick(TickState const& state)
        {
            auto& in = state.inputs[0];
            Sample sample = in.get();
            for (auto& out : state.outputs)
            {
                out.push(sample);
            }
        }
    };

    struct DetachWriterNode {
        size_t id;

        struct State {
            Sample* slot{};
        };

        constexpr auto inputs() const {
            return std::array<InputConfig, 1>{};
        }

        template<class Alloc, class Ctx>
        void init_buffer(Alloc& alloc, Ctx& ctx) const
        {
            State& st = alloc.template new_object<State>();
            st.slot = ctx.acquire_detach_slot(id, alloc);
        }

        void tick(TickState const& ts) const
        {
            auto& st = ts.get_state<State>();
            *st.slot = ts.inputs[0].get();
        }
    };

    struct DetachReaderNode {
        size_t id;

        struct State {
            Sample* slot{};
        };

        constexpr auto outputs() const {
            return std::array<OutputConfig, 1>{};
        }

        template<class Alloc, class Ctx>
        void init_buffer(Alloc& alloc, Ctx& ctx) const
        {
            State& st = alloc.template new_object<State>();
            st.slot = ctx.acquire_detach_slot(id, alloc);
        }

        void tick(TickState const& ts) const
        {
            auto& st = ts.get_state<State>();
            ts.outputs[0].push(*st.slot);
        }
    };

    struct DummySink {
        constexpr auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        void tick(TickState const&) const
        {}
    };

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

        void tick(TickState const& state)
        {
            auto const& in = state.inputs[0];
            auto const& in_threshold = state.inputs[1];
            auto const& out = state.outputs[0];
            auto const& out_aliased = state.outputs[1];

            Sample threshold = in_threshold.get();
            Sample sample_prev = std::fmin(std::fmax(out_aliased.get(), -1.0), 1.0);
            Sample sample = std::fmin(std::fmax(in.get(), -1.0), 1.0);
            Sample sample_warped = sample;
            bool warped = false;

            if (sample > threshold) { sample_warped = warp_pm1(sample, threshold); warped = true; }
            else if (sample < -threshold) { sample_warped = warp_pm1(sample, threshold); warped = true; }
            sample_warped = std::fmin(std::fmax(sample_warped, -1.0), 1.0);
            out_aliased.push(sample_warped);

            Sample sample_warped_aa = sample_warped;
            if (warped) {
                Sample delta = Sample((sample - sample_prev) / 2.0);
                out.update(sample_prev - polyblep_error(sample_prev, delta, threshold, PolyblepSide::LEFT));
                sample_warped_aa -= polyblep_error(sample_warped_aa, delta, threshold, PolyblepSide::RIGHT);
            }
            out.push(std::fmin(std::fmax(sample_warped_aa, -1.0), 1.0));
        }
    };

    struct Integrator {
        constexpr explicit Integrator()
        {}

        constexpr auto inputs() const
        {
            return std::array {
                InputConfig { "f_prev" },
                InputConfig { "f"},
                InputConfig { .name = "dx", .default_value = 1.0 },
            };
        }

        constexpr auto outputs() const
        {
            return std::array {
                OutputConfig { "integral" },
            };
        }

        void tick(iv::TickState const& state)
        {
            auto& out = state.outputs[0];
            auto const f_prev = state.inputs[0].get();
            auto const f = state.inputs[1].get();
            auto const dx = state.inputs[2].get();
            out.push(warp_pm1(f_prev + f * dx, 1.0));
        }
    };

    class Latency {
        size_t _latency;

    public:
        constexpr explicit Latency(size_t latency = 1) :
            _latency(latency)
        {}

        constexpr auto inputs() const
        {
            return std::array<iv::InputConfig, 1>{};
        }

        constexpr auto outputs() const
        {
            return std::array{
                iv::OutputConfig { .latency = _latency },
            };
        }

        void tick(iv::TickState const& state)
        {
            auto const& in = state.inputs[0];
            auto const& out = state.outputs[0];
            out.push(in.get());
        }
    };

    struct Constant {
        Sample _value;

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        constexpr void tick(TickState const& state)
        {
            auto& out = state.outputs[0];
            out.push(_value);
        }
    };
    
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

        void tick(TickState const& state)
        {
            if (!_generator.has_value()) {
                _generator.emplace(_seed.has_value() ? _seed.value() : std::random_device{}());
                _distribution.emplace(_min, _max);
            }
            auto& out = state.outputs[0];
            out.push((*_distribution)(*_generator));
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

        double uniform_m11(uint64_t i, Sample min, Sample max) const {
            // 1) harvest top 52 bits of i → mantissa
            uint64_t mantissa = i >> (64 - 52);

            // 2) set exponent to (bias+1)=1024 → raw range [2.0, 4.0)
            //    exponent bits = 0x400, so the constant is 0x4000000000000000ULL
            uint64_t bits = 0x4000000000000000ULL | mantissa;

            // 3) reinterpret as double (in [2,4)), then subtract 3.0 → [-1,1)
            double range = (max - min)/2.0;
            double min_reinterpret = 2.0*min - max;
            return std::bit_cast<double>(bits)*range + min_reinterpret;
        }

    public:
        constexpr explicit DeterministicUniformNoise(
            std::optional<Sample> seed = {}
        ):
            _seed(seed.has_value()
                ? *seed
                : (static_cast<std::uint64_t>(std::random_device{}()) << 32) |
                  static_cast<std::uint64_t>(std::random_device{}()))
        {}

        constexpr auto inputs() const
        {
            return std::array{
                InputConfig { .name = "min", .default_value = -1.0 },
                InputConfig { .name = "max", .default_value = 1.0 },
            };
        }

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickState const& state)
        {
            auto& out = state.outputs[0];
            auto const min = state.inputs[0].get();
            auto const max = state.inputs[1].get();
            uint64_t uniform_int = splitmix64(state.index);
            Sample uniform_float = uniform_m11(uniform_int, min, max);
            out.push(uniform_float);
        }
    };

    class DeterministicUniformAESNoise {
        using Rng = r123::AESNI4x32;
        Rng _generator;
        Rng::key_type _seed;

        static Rng::key_type make_seed(std::optional<uint64_t> seed_opt)
        {
            uint32_t seed_low, seed_high;
            if (seed_opt.has_value())
            {
                uint64_t seed = *seed_opt;
                seed_low = static_cast<uint32_t>(seed);
                seed_high = static_cast<uint32_t>(seed >> 32);
            }
            else
            {
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
        explicit DeterministicUniformAESNoise(
            std::optional<uint64_t> seed = {}
        ) :
            _seed(make_seed(seed))
        {
            assert(haveAESNI() && "This machine does not have the AES-NI instruction set, use a different noise node.");
        }

        constexpr auto inputs() const
        {
            return std::array{
                InputConfig { .name = "min", .default_value = -1.0 },
                InputConfig { .name = "max", .default_value = 1.0 },
            };
        }

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickState const& state)
        {
            auto& out = state.outputs[0];
            auto const min = state.inputs[0].get();
            auto const max = state.inputs[1].get();
            Rng::ctr_type counter = make_index(state.index);
            unsigned int uniform_uint = _generator(counter, _seed)[0];
            Sample uniform_float = r123::u01<Sample>(uniform_uint) * (max - min) + min;
            out.push(uniform_float);
        }
    };

    class UniformToCauchy {
        Sample _x0;
        Sample _gamma;

    public:
        explicit UniformToCauchy(
            Sample x0 = 1.0,
            Sample gamma = 1.0
        ) :
            _x0(x0),
            _gamma(gamma)
        {}

        constexpr auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickState const& state)
        {
            auto& in = state.inputs[0];
            auto& out = state.outputs[0];
            Sample uniform = in.get();
            Sample cauchy = _x0 + _gamma * std::tanf(std::numbers::pi_v<float> * uniform * 0.5);
            out.push(cauchy);
        }
    };

    class UniformToPower {
        ptrdiff_t _min;
        ptrdiff_t _max;
        Sample _lambda;

        struct State {
            std::span<Sample> weights;
        };

    public:
        explicit UniformToPower(
            ptrdiff_t min = -5,
            ptrdiff_t max = 4,
            Sample lambda = 0.5
        ) :
            _min(min),
            _max(max),
            _lambda(lambda)
        {}

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
            for (size_t i = 0; i < range; ++i)
            {
                alloc.assign(alloc.at(s.weights, i), std::powf(_lambda, range - 1 - static_cast<ptrdiff_t>(i)));
            }
            if (alloc.can_allocate())
            {
                Sample total = 0.0;
                for (size_t i = 0; i < range; ++i)
                {
                    total += s.weights[i];
                }
                for (size_t i = 0; i < range; ++i)
                {
                    s.weights[i] /= total;
                }
            }
        }

        void tick(TickState const& state)
        {
            auto& in = state.inputs[0];
            auto& out = state.outputs[0];
            auto& s = state.get_state<State>();
            Sample uniform = in.get() * 0.5 + 0.5;
            size_t discrete = static_cast<size_t>(std::lower_bound(s.weights.begin(), s.weights.end(), uniform) - s.weights.begin());
            out.push(std::exp2f(static_cast<Sample>(discrete)));
        }
    };

    class UniformToGaussian {
        Sample _mean;
        Sample _std;

    public:
        explicit UniformToGaussian(
            Sample mean = 0.0,
            Sample std = 1.0
        ) :
            _mean(mean),
            _std(std)
        {
        }

        constexpr auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickState const& state)
        {
            auto& in = state.inputs[0];
            auto& out = state.outputs[0];
            Sample uniform = in.get();
            Sample normal = std::numbers::sqrt2_v<float> * erfinvf(uniform);
            Sample gaussian = std::fmaf(normal, _std, _mean);
            out.push(gaussian);
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
            uint32_t seed_low, seed_high;
            if (seed_opt.has_value())
            {
                uint64_t seed = *seed_opt;
                seed_low = static_cast<uint32_t>(seed);
                seed_high = static_cast<uint32_t>(seed >> 32);
            }
            else
            {
                seed_low = std::random_device{}();
                seed_high = std::random_device{}();
            }
            return Rng::ukey_type{ seed_low, seed_high, 0, 0 };
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
        ) :
            _seed(make_seed(seed)),
            _mean(mean),
            _std(std)
        {
            assert(haveAESNI() && "This machine does not have the AES-NI instruction set, use a different noise node.");
        }

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickState const& state)
        {
            auto& out = state.outputs[0];
            Rng::ctr_type counter = make_index(state.index);
            unsigned int uniform_uint = _generator(counter, _seed)[0];
            Sample uniform = r123::uneg11<Sample>(uniform_uint);
            Sample gaussian = std::numbers::sqrt2_v<float> * erfinvf(uniform);
            gaussian = std::fmaf(gaussian, _std, _mean);
            out.push(gaussian);
        }
    };

    class NlmsPredictor {
        size_t _look_ahead;
        size_t _order;
        float _lr;
        float _decay;

        struct State {
            std::span<Sample> w;
        };

    public:
        constexpr NlmsPredictor(
            size_t look_ahead,
            size_t order,
            float lr = 1e-4,
            float decay = 1.0
        )
            : _look_ahead(look_ahead)
            , _order(order)
            , _lr(lr)
            , _decay(decay)
        {
            assert(_order >= _look_ahead && "window length must cover look-ahead");
        }

        auto inputs() const
        {
            return std::array {
                InputConfig { .history = _order - 1 }
            };
        }

        auto outputs() const
        {
            return std::array {
                OutputConfig { .history = _look_ahead } // must recall past predictions
            };
        }
        
        template<typename Alloc>
        void init_buffer(Alloc& alloc) const
        {
            State& st = alloc.template new_object<State>();
            alloc.assign(st.w, alloc.template new_array<float>(_order));
            alloc.fill_n(st.w, 0.f);              // cold-start weights
            alloc.assign(alloc.at(st.w, 0),  1.f);
        }

        void tick(TickState const& ts) const
        {
            State& st = ts.get_state<State>();
            auto  &in  = ts.inputs[0];
            auto  &out = ts.outputs[0];

            /* ---------- form prediction y(n) ---------------------- */
            float y = 0.f;
            for (size_t k = 0; k < _order; ++k)
                y += st.w[k] * in.get(k);        // x(n-L-k)

            /* deliver prediction this sample */
            out.push(y);

            /* ---------- LMS adaptation, executes L ticks later ---- */
            float real = in.get(0);
            float past_pred = out.get(_look_ahead);
            float err = real - past_pred;

            /* normalisation */
            float norm = 1e-6f;
            for (size_t k = 0; k < _order; ++k) {
                float s = in.get(k);
                norm += s * s;
            }
            // const float beta = 0.01f;
            /*
            float mu_dyn = _learning_rate * (1.f + beta * err * err / norm);
            mu_dyn = std::clamp(mu_dyn, _learning_rate, 4.f * _learning_rate);*/

            // float err_clip = 0.1f;
            //if (fabs(err) > err_clip) err = copysign(err_clip, err);

            float g = _lr * err / norm;

            /* w ← w + g * x_vec */
            for (size_t k = 0; k < _order; ++k)
                st.w[k] = st.w[k] * _decay + g * in.get(k);
        }
    };

    class TanhResidualPredictor {
        size_t _L;       // look-ahead (latency still present on edge)
        size_t _p;       // FIR window taps
        size_t _q;       // AR taps (past residual outputs)
        size_t _h;       // hidden units
        float  _mu;      // base learning-rate

        struct State {
            std::span<float> W1;   // h × (p+q)
            std::span<float> W2;   // h
            std::span<float> b1;
            std::span<float> a;    // scratch h
        };

    public:
        constexpr TanhResidualPredictor(
            size_t look_ahead,
            size_t order,
            size_t ar_order = 2,
            size_t hidden = 8,
            float  mu = 1e-6f)
            : _L(look_ahead)
            , _p(order)
            , _q(ar_order)
            , _h(hidden)
            , _mu(mu)
        {
            assert(_p >= _L && "window length must cover look-ahead");
        }

        /* graph meta-data */
        auto inputs()  const {
            return std::array {            // need x[n-L] … x[n-L-(p-1)]
                InputConfig {.history = _p - 1 }
            };
        }
        auto outputs() const {
            return std::array {
                OutputConfig { .history = _L + _q }   // recall ŷ[n-L]
            };
        }

        template<typename Allocator>
        void init_buffer(Allocator& allocator) const
        {
            State& s = allocator.template new_object<State>();
            allocator.assign(s.W1, allocator.template new_array<float>(_h * (_p + _q)));
            allocator.assign(s.W2, allocator.template new_array<float>(_h));
            allocator.assign(s.b1, allocator.template new_array<float>(_h));
            allocator.assign(s.a, allocator.template new_array<float>(_h));
            allocator.fill_n(s.W1, 0.f);
            allocator.fill_n(s.W2, 0.f);
            allocator.fill_n(s.b1, 0.f);
        }

        void tick(TickState const& ts) const
        {
            State& s = ts.get_state<State>();
            auto& in = ts.inputs[0];
            auto& out = ts.outputs[0];

            auto x = [&](size_t k) { return in.get(k);            }; // 0…p-1
            auto r_prev = [&](size_t j) { return out.get(_L + j); }; // 1…q

            /* -------- baseline:  pure delay ----------------------- */
            float y0 = x(0);  // x[n-L]

            /* -------- NN predicts residual r̂ --------------------- */
            /* hidden layer */
            for (size_t i = 0; i < _h; ++i) {
                float z = s.b1[i];
                const float* w = &s.W1[i * (_p + _q)];

                /* FIR part */
                for (size_t k = 0; k < _p; ++k) z += w[k] * x(k);

                /* AR part */
                for (size_t j = 1; j <= _q; ++j) z += w[_p + (j - 1)] * r_prev(j);

                s.a[i] = std::tanh(z);
            }
            float r_hat = 0.f;
            for (size_t i = 0; i < _h; ++i) r_hat += s.W2[i] * s.a[i];

            /* combined prediction */
            float y = y0 + r_hat;
            out.push(y);                               // stores ŷ[n-L] at write-head

            /* -------- error computed L ticks later --------------- */
            float real = in.get(0);               // now x[n-L]
            float past_pred = r_prev(0);          // out.get(_L)
            float err = real - past_pred;         // residual error

            /* clip huge spikes (saw reset) */
            //err = std::clamp(err, -1.f, 1.f);

            /* variable step (optional) */
            //float mu = std::min(4 * _mu, _mu * (1.f + 0.01f * err * err));

            /* update W2 */
            for (size_t i = 0; i < _h; ++i)         // W2
                s.W2[i] += _mu * err * s.a[i];

            for (size_t i = 0; i < _h; ++i) {        // W1 + b1
                float delta = (s.W2[i] * err) * (1.f - s.a[i] * s.a[i]); // tanh'
                float* w = &s.W1[i * (_p + _q)];

                for (size_t k = 0; k < _p; ++k)
                    w[k] += _mu * delta * x(k);            // FIR taps
                for (size_t j = 1; j <= _q; ++j)
                    w[_p + (j - 1)] += _mu * delta * r_prev(j);// AR taps

                s.b1[i] += _mu * delta;
            }
        }
    };

    class TanhResidualAR2Predictor {
        size_t _L;               // look-ahead
        size_t _p;               // FIR taps
        size_t _q;               // AR taps
        size_t _h1, _h2;         // hidden-layer sizes
        float  _mu;              // learning rate

        struct State {
            std::span<float> W1, b1;      // h1 × (p+q)   , h1
            std::span<float> W2, b2;      // h2 × h1      , h2
            std::span<float> W3;          // h2
            std::span<float> a1, a2;      // scratch: h1 , h2
        };

        template<typename Buf>
        static State& st(Buf b) {
            void* o = b.data(); size_t s = b.size();
            return *reinterpret_cast<State*>(std::align(
                alignof(State), sizeof(State), o, s));
        }

    public:
        constexpr TanhResidualAR2Predictor(size_t  look_ahead,
            size_t  order,
            size_t  ar_order = 2,
            size_t  hidden1 = 16,
            size_t  hidden2 = 8,
            float   mu = 2e-6f)
            : _L(look_ahead), _p(order), _q(ar_order),
            _h1(hidden1), _h2(hidden2), _mu(mu)
        {
            assert(_p >= _L);
        }

        /* ───── graph meta-data ───── */
        auto inputs()  const {
            return std::array{
                InputConfig{.history = _p - 1 }
            };
        }
        auto outputs() const {
            return std::array{
                OutputConfig{.latency = 0, .history = _L + _q }
            };
        }

        /* ───── buffer reservation ───── */
        template<typename A>
        void init_buffer(A& alloc) const
        {
            State& s = alloc.template new_object<State>();

            alloc.assign(s.W1, alloc.template new_array<float>(_h1 * (_p + _q)));
            alloc.assign(s.b1, alloc.template new_array<float>(_h1));

            alloc.assign(s.W2, alloc.template new_array<float>(_h2 * _h1));
            alloc.assign(s.b2, alloc.template new_array<float>(_h2));

            alloc.assign(s.W3, alloc.template new_array<float>(_h2));

            alloc.assign(s.a1, alloc.template new_array<float>(_h1));
            alloc.assign(s.a2, alloc.template new_array<float>(_h2));

            alloc.fill_n(s.W1, 0.f); alloc.fill_n(s.W2, 0.f); alloc.fill_n(s.W3, 0.f);
            alloc.fill_n(s.b1, 0.f); alloc.fill_n(s.b2, 0.f);
        }

        /* ───── processing ───── */
        void tick(TickState const& ts) const
        {
            State& s = st(ts.buffer);
            auto& in = ts.inputs[0];
            auto& out = ts.outputs[0];

            auto x = [&](size_t k) { return in.get(k); };           // 0…p-1
            auto r_p = [&](size_t j) { return out.get(_L + j); };     // 1…q

            /* 1. baseline delay */
            float y0 = x(0);                                  // x[n-L]

            /* 2. hidden layer 1 */
            for (size_t i = 0; i < _h1; ++i) {
                float z = s.b1[i];
                const float* w = &s.W1[i * (_p + _q)];
                for (size_t k = 0; k < _p; ++k)      z += w[k] * x(k);
                for (size_t j = 0; j < _q; ++j)     z += w[_p + j] * r_p(j + 1);
                s.a1[i] = std::tanh(z);
            }

            /* 3. hidden layer 2 */
            for (size_t i = 0; i < _h2; ++i) {
                float z = s.b2[i];
                const float* w = &s.W2[i * _h1];
                for (size_t k = 0; k < _h1; ++k) z += w[k] * s.a1[k];
                s.a2[i] = std::tanh(z);
            }

            /* 4. linear read-out (no bias) */
            float r_hat = 0.f;
            for (size_t i = 0; i < _h2; ++i) r_hat += s.W3[i] * s.a2[i];

            float y = y0 + r_hat;
            out.push(y);                           // ŷ[n-L]

            /* 5. error for sample x[n-L] */
            float real = x(0);
            float past_pred = r_p(0);              // out.get(_L)
            float err = std::clamp(real - past_pred, -1.f, 1.f);

            /* 6. update W3 */
            for (size_t i = 0; i < _h2; ++i)
                s.W3[i] += _mu * err * s.a2[i];

            /* 7. back-prop layer 2 */
            for (size_t i = 0; i < _h2; ++i) {
                float delta2 = (s.W3[i] * err) * (1.f - s.a2[i] * s.a2[i]);   // tanh'
                float* w2 = &s.W2[i * _h1];
                for (size_t k = 0; k < _h1; ++k)
                    w2[k] += _mu * delta2 * s.a1[k];
                s.b2[i] += _mu * delta2;

                /* stash delta2 in a2 for layer-1 back-prop reuse */
                s.a2[i] = delta2;
            }

            /* 8. back-prop layer 1 */
            for (size_t k1 = 0; k1 < _h1; ++k1) {
                float sum = 0.f;
                for (size_t i = 0; i < _h2; ++i)
                    sum += s.W2[i * _h1 + k1] * s.a2[i];   // a2 now holds delta2
                float delta1 = sum * (1.f - s.a1[k1] * s.a1[k1]);
                float* w1 = &s.W1[k1];                   // stride (_p+_q)

                for (size_t k = 0; k < _p; ++k)
                    w1[k] += _mu * delta1 * x(k);
                for (size_t j = 0; j < _q; ++j)
                    w1[_p + j] += _mu * delta1 * r_p(j + 1);

                s.b1[k1] += _mu * delta1;
            }
        }
    };

    class PolyResidualPredictor {
        size_t _L;              // look-ahead still on edge
        size_t _p;              // window length
        float  _mu;             // learning rate

        struct State {
            std::span<float> w;         // length 2·p   [w_lin | w_quad]
        };

        template<typename Buf>
        static State& st(Buf b) {
            void* o = b.data(); size_t s = b.size();
            return *reinterpret_cast<State*>(std::align(
                alignof(State), sizeof(State), o, s));
        }

    public:
        constexpr PolyResidualPredictor(size_t look_ahead,
            size_t order,
            float  mu = 1e-5f
        )
            : _L(look_ahead)
            , _p(order)
            , _mu(mu)
        {
            assert(_p >= _L);
        }

        /* ─── meta-data ─── */
        auto inputs()  const {
            return std::array {
                InputConfig {.history = _p - 1 },
            };
        }
        auto outputs() const {
            return std::array {
                OutputConfig { .history = _L },
            };
        }

        /* ─── allocation ─── */
        template<typename A>
        void init_buffer(A& alloc) const
        {
            State& s = alloc.template new_object<State>();
            alloc.assign(s.w, alloc.template new_array<float>(2 * _p));
            alloc.fill_n(s.w, 0.f);
        }

        /* ─── processing ─── */
        void tick(TickState const& ts) const
        {
            State& s = st(ts.buffer);
            auto& in = ts.inputs[0];
            auto& out = ts.outputs[0];

            auto x = [&](size_t k) { return in.get(k); };   // x[n-L-k]

            /* baseline delay */
            float y0 = x(0);

            /* ---- forward: linear + quad terms ------------------- */
            float r_hat = 0.f;
            for (size_t k = 0; k < _p; ++k) {
                float xk = x(k);
                float* wk = &s.w[2 * k];          // w_lin , w_quad
                r_hat += wk[0] * xk + wk[1] * xk * xk;
            }

            float y = y0 + r_hat;
            out.push(y);                        // ŷ[n-L]

            /* ---- error for sample x[n-L] ------------------------ */
            float real = x(0);
            float past_pred = out.get(_L);      // ŷ[n-L] made L ticks ago
            float err = std::clamp(real - past_pred, -1.f, 1.f);

            /* ---- norm for NLMS (include quad term magnitude) ---- */
            float norm = 1e-6f;
            for (size_t k = 0; k < _p; ++k) {
                float xk = x(k);
                norm += xk * xk + (xk * xk) * (xk * xk);   // |[x, x²]|²
            }

            float g = _mu * err / norm;

            /* ---- weight update ---------------------------------- */
            for (size_t k = 0; k < _p; ++k) {
                float xk = x(k);
                float* wk = &s.w[2 * k];
                wk[0] += g * xk;          // linear term
                wk[1] += g * xk * xk;     // quadratic term
            }
        }
    };

    class TypeErasedNode {
        std::shared_ptr<void> _node;
        std::vector<InputConfig> _inputs;
        std::vector<OutputConfig> _outputs;
        size_t _internal_latency;
        std::span<std::byte>(*_init_buffer_fn)(void*, TypeErasedAllocator, GraphInitContext&);
        void (*_tick_fn)(void*, TickState const&);

    public:
        template<typename Node>
        constexpr /*implicit*/ TypeErasedNode(Node node)
        {
            if constexpr (std::is_empty_v<Node>)
            {
                _node = nullptr;
                _init_buffer_fn = [](void*, TypeErasedAllocator allocator, GraphInitContext& ctx) { return do_init_buffer(Node{}, allocator, ctx); };
                _tick_fn = [](void*, TickState const& state) { Node{}.tick(state); };
            }
            else
            {
                _node = std::make_shared<Node>(node);
                _init_buffer_fn = [](void* node, TypeErasedAllocator allocator, GraphInitContext& ctx) { return do_init_buffer(*static_cast<Node*>(node), allocator, ctx); };
                _tick_fn = [](void* node, TickState const& state) { static_cast<Node*>(node)->tick(state); };
            }
            _inputs.assign_range(get_inputs(node));
            _outputs.assign_range(get_outputs(node));
            _internal_latency = get_internal_latency(node);
        }

        constexpr std::span<InputConfig const> inputs() const
        {
            return _inputs;
        }

        constexpr std::span<OutputConfig const> outputs() const
        {
            return _outputs;
        }

        constexpr size_t internal_latency() const
        {
            return _internal_latency;
        }

        template<typename Allocator>
        constexpr std::span<std::byte> init_buffer(Allocator& allocator, GraphInitContext& ctx) const
        {
            return _init_buffer_fn(_node.get(), TypeErasedAllocator{ allocator }, ctx);
        }

        void tick(TickState const& state)
        {
            _tick_fn(_node.get(), state);
        }
    };
    
    struct Interpolation {
        constexpr auto inputs() const
        {
            return std::array{
                InputConfig { "a" },
                InputConfig { "b" },
                InputConfig { "alpha" },
            };
        }

        constexpr auto outputs() const
        {
            return std::array {
                OutputConfig { "out" },
            };
        }

        void tick(TickState const& state)
        {
            Sample a = state.inputs[0].get();
            Sample b = state.inputs[1].get();
            Sample alpha = state.inputs[2].get();
            auto& out = state.outputs[0];
            out.push(a + alpha*(b-a));
        }
    };

    class BufferSink {
        iv::Sample* _destination;
        size_t _size;

    public:
        constexpr explicit BufferSink(
            iv::Sample* destination,
            size_t size
        ):
            _destination(destination),
            _size(size)
        {}

        constexpr auto inputs() const
        {
            return std::array<iv::InputConfig, 1>{};
        }

        void tick(iv::TickState const& state)
        {
            auto& in = state.inputs[0];
            if (state.index < _size) _destination[state.index] = in.get();
        }
    };

    class BufferSource {
        iv::Sample* _source;
        size_t _size;

    public:
        constexpr explicit BufferSource(
            iv::Sample* source,
            size_t size
        ):
            _source(source),
            _size(size)
        {}

        constexpr auto outputs() const
        {
            return std::array<iv::OutputConfig, 1>{};
        }

        void tick(iv::TickState const& state)
        {
            auto& out = state.outputs[0];
            if (state.index < _size) {
                out.push(_source[state.index]);
            }
            else {
                out.push(_source[_size-1]);
            }
        }
    };

    struct WhackIirThing {
        constexpr explicit WhackIirThing()
        {}

        constexpr auto inputs() const
        {
            return std::array<iv::InputConfig, 3>{};
        }

        constexpr auto outputs() const
        {
            return std::array<iv::OutputConfig, 2>{};
        }

        void tick(iv::TickState const& state) const
        {
            iv::Sample in_dry = state.inputs[0].get();
            iv::Sample in_control = state.inputs[1].get();
            iv::Sample alpha = 1 - in_control; for (size_t i = 0; i < 4; ++i) alpha *= alpha;

            auto& out_low = state.outputs[0];
            auto& out_high = state.outputs[1];
            iv::Sample last_low = out_low.get();

            iv::Sample low = last_low + alpha * (in_dry - last_low);
            out_low.push(low);
            out_high.push(in_dry - low);
        }
    };

    struct SimpleIirHighPass {
        static constexpr iv::Sample const FMIN = 2e1;
        static constexpr iv::Sample const FMAX = 2e4;

        constexpr explicit SimpleIirHighPass()
        {}

        constexpr auto inputs() const
        {
            return std::array {
                iv::InputConfig { .name = "in", .history = 1 },
                iv::InputConfig { .name = "cutoff" },
                iv::InputConfig { .name = "dx", .default_value = 1.0 },
            };
        }

        constexpr auto outputs() const
        {
            return std::array<iv::OutputConfig, 1>{};
        }

        void tick(iv::TickState const& state) const
        {
            auto& in = state.inputs[0];
            auto const ctrl = state.inputs[1].get();          // ∈ [0,1]
            auto const dx = state.inputs[2].get();
            auto& out = state.outputs[0];

            // compute host the *usable* max cutoff
            auto const usableMax = std::min<Sample>(FMAX, 0.5 / dx);

            // 2) (optionally) exponential sweep, clamped
            auto const f_c = FMIN * std::pow(usableMax / FMIN, ctrl);

            // normalize and compute IIR coeffs
            auto const norm = f_c * dx;                     // ∈ [0, usableMax/fs] ⩽ 0.5
            auto const c = std::tan(std::_Pi_val * norm);
            auto const a1 = (1.0 - c) / (1.0 + c);
            auto const b0 = 1.0 / (1.0 + c);

            // standard one‑pole highpass:
            auto const x = in.get(0);
            auto const x1 = in.get(1);
            auto const y1 = out.get();
            auto const y = a1*y1 + b0*(x - x1);

            out.push(y);
        }
    };

    struct SimpleIirLowPass {
        static constexpr iv::Sample const FMIN = 2e1;
        static constexpr iv::Sample const FMAX = 2e4;

        constexpr explicit SimpleIirLowPass()
        {}

        constexpr auto inputs() const
        {
            return std::array {
                iv::InputConfig { .name = "in", .history = 1 },
                iv::InputConfig { .name = "cutoff" },
                iv::InputConfig { .name = "dx", .default_value = 1.0 },
            };
        }

        constexpr auto outputs() const
        {
            return std::array {
                iv::OutputConfig { "out" },
            };
        }

        void tick(iv::TickState const& state) const
        {
            auto& in_port = state.inputs[0];
            auto const ctrl = state.inputs[1].get();          // ∈ [0,1]
            auto const dx = state.inputs[2].get();
            auto& out = state.outputs[0];

            // 2) compute sample‐rate and clamped max cutoff
            auto const usableMax = std::min<Sample>(FMAX, 0.5 / dx);

            // 3a) linear sweep: maps 0→1 straight to FMIN→usableMax
            //iv::Sample fc_linear = FMIN + u * (usableMax - FMIN);

            // 3b) exponential sweep: perceptually smoother
            auto const f_c = FMIN * std::pow(usableMax / FMIN, ctrl);

            // 4) normalize to [0…0.5] and compute warped c
            auto const norm = f_c * dx;             // now ∈ [FMIN/fs … usableMax/fs] ⩽ 0.5
            auto const c = std::tan(std::_Pi_val * norm);

            // 5) one‑pole low‑pass coeffs
            auto const a1 = (1.0 - c) / (1.0 + c);
            auto const alpha = c / (1.0 + c);

            // 6) apply difference equation
            auto const x = in_port.get(0);
            auto const x_prev = in_port.get(1);
            auto const y_prev = out.get();

            auto const y = a1*y_prev + alpha*(x + x_prev);

            out.push(y);
        }
    };
}
