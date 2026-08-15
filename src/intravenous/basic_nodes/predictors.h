#pragma once

#include <intravenous/node/lifecycle.h>

#include <cmath>
#include <ranges>
#include <array>
#include <cassert>

namespace iv {
    template<size_t LookAhead, size_t Order>
    class NlmsPredictor {
        Sample _lr;
        Sample _decay;

    public:
        struct State {
            std::span<Sample> w;
        };

        NlmsPredictor(Sample lr = 1e-4, Sample decay = 1.0) :
            _lr(lr),
            _decay(decay)
        {
            static_assert(Order >= LookAhead, "window length must cover look-ahead");
        }

        static constexpr auto inputs()
        {
            return std::array { InputConfig { .history = Order - 1 } };
        }

        static constexpr auto outputs()
        {
            return std::array { OutputConfig { .history = LookAhead } };
        }

        void declare(DeclarationContext<NlmsPredictor> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.w, Order);
        }

        void initialize(InitializationContext<NlmsPredictor> const& ctx) const
        {
            auto& state = ctx.state();
            std::ranges::fill(state.w, Sample{});
            state.w[0] = 1.f;
        }

        void tick(TickSampleContext<NlmsPredictor> const& ctx) const
        {
            auto& state = ctx.state();
            auto& in = ctx.inputs[0];
            auto& out = ctx.outputs[0];

            Sample y = 0.f;
            for (size_t k = 0; k < Order; ++k) {
                y += state.w[k] * in.get(k);
            }
            out.push(y);

            Sample real = in.get(0);
            Sample past_pred = out.get(LookAhead);
            Sample err = real - past_pred;

            Sample norm = 1e-6f;
            for (size_t k = 0; k < Order; ++k) {
                Sample s = in.get(k);
                norm += s * s;
            }

            Sample g = _lr * err / norm;
            for (size_t k = 0; k < Order; ++k) {
                state.w[k] = state.w[k] * _decay + g * in.get(k);
            }
        }
    };

    template<size_t LookAhead, size_t Order, size_t ArOrder = 2, size_t Hidden = 8>
    class TanhResidualPredictor {
        Sample _mu;

    public:
        struct State {
            std::span<Sample> W1;
            std::span<Sample> W2;
            std::span<Sample> b1;
            std::span<Sample> a;
        };

        TanhResidualPredictor(Sample mu = 1e-6f) :
            _mu(mu)
        {
            static_assert(Order >= LookAhead, "window length must cover look-ahead");
        }

        static constexpr auto inputs()
        {
            return std::array { InputConfig { .history = Order - 1 } };
        }

        static constexpr auto outputs()
        {
            return std::array { OutputConfig { .history = LookAhead + ArOrder } };
        }

        void declare(DeclarationContext<TanhResidualPredictor> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.W1, Hidden * (Order + ArOrder));
            ctx.local_array(state.W2, Hidden);
            ctx.local_array(state.b1, Hidden);
            ctx.local_array(state.a, Hidden);
        }

        void initialize(InitializationContext<TanhResidualPredictor> const& ctx) const
        {
            State& state = ctx.state();
            std::ranges::fill(state.W1, Sample{});
            std::ranges::fill(state.W2, Sample{});
            std::ranges::fill(state.b1, Sample{});
        }

        void tick(TickSampleContext<TanhResidualPredictor> const& ctx) const
        {
            State& s = ctx.state();
            auto& in = ctx.inputs[0];
            auto& out = ctx.outputs[0];

            auto x = [&](size_t k) { return in.get(k); };
            auto r_prev = [&](size_t j) { return out.get(LookAhead + j); };

            Sample y0 = x(0);

            for (size_t i = 0; i < Hidden; ++i) {
                Sample z = s.b1[i];
                const Sample* w = &s.W1[i * (Order + ArOrder)];
                for (size_t k = 0; k < Order; ++k) {
                    z += w[k] * x(k);
                }
                for (size_t j = 1; j <= ArOrder; ++j) {
                    z += w[Order + (j - 1)] * r_prev(j);
                }
                s.a[i] = std::tanh(z);
            }

            Sample r_hat = 0.f;
            for (size_t i = 0; i < Hidden; ++i) {
                r_hat += s.W2[i] * s.a[i];
            }

            Sample y = y0 + r_hat;
            out.push(y);

            Sample real = in.get(0);
            Sample past_pred = r_prev(0);
            Sample err = real - past_pred;

            for (size_t i = 0; i < Hidden; ++i) {
                s.W2[i] += _mu * err * s.a[i];
            }

            for (size_t i = 0; i < Hidden; ++i) {
                Sample delta = (s.W2[i] * err) * (1.f - s.a[i] * s.a[i]);
                Sample* w = &s.W1[i * (Order + ArOrder)];
                for (size_t k = 0; k < Order; ++k) {
                    w[k] += _mu * delta * x(k);
                }
                for (size_t j = 1; j <= ArOrder; ++j) {
                    w[Order + (j - 1)] += _mu * delta * r_prev(j);
                }
                s.b1[i] += _mu * delta;
            }
        }
    };

    template<size_t LookAhead, size_t Order, size_t ArOrder = 2, size_t Hidden1 = 16, size_t Hidden2 = 8>
    class TanhResidualAR2Predictor {
        Sample _mu;

    public:
        struct State {
            std::span<Sample> W1, b1;
            std::span<Sample> W2, b2;
            std::span<Sample> W3;
            std::span<Sample> a1, a2;
        };

        TanhResidualAR2Predictor(Sample mu = 2e-6f)
        : _mu(mu)
        {
            static_assert(Order >= LookAhead, "window length must cover look-ahead");
        }

        static constexpr auto inputs()
        {
            return std::array { InputConfig { .history = Order - 1 } };
        }

        static constexpr auto outputs()
        {
            return std::array { OutputConfig { .latency = 0, .history = LookAhead + ArOrder } };
        }

        void declare(DeclarationContext<TanhResidualAR2Predictor> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.W1, Hidden1 * (Order + ArOrder));
            ctx.local_array(state.b1, Hidden1);
            ctx.local_array(state.W2, Hidden2 * Hidden1);
            ctx.local_array(state.b2, Hidden2);
            ctx.local_array(state.W3, Hidden2);
            ctx.local_array(state.a1, Hidden1);
            ctx.local_array(state.a2, Hidden2);
        }

        void initialize(InitializationContext<TanhResidualAR2Predictor> const& ctx) const
        {
            auto& state = ctx.state();
            std::ranges::fill(state.W1, Sample{});
            std::ranges::fill(state.W2, Sample{});
            std::ranges::fill(state.W3, Sample{});
            std::ranges::fill(state.b1, Sample{});
            std::ranges::fill(state.b2, Sample{});
        }

        void tick(TickSampleContext<TanhResidualAR2Predictor> const& ctx) const
        {
            State& state = ctx.state();
            auto& in = ctx.inputs[0];
            auto& out = ctx.outputs[0];

            auto x = [&](size_t k) { return in.get(k); };
            auto r_p = [&](size_t j) { return out.get(LookAhead + j); };

            Sample y0 = x(0);

            for (size_t i = 0; i < Hidden1; ++i) {
                Sample z = state.b1[i];
                const Sample* w = &state.W1[i * (Order + ArOrder)];
                for (size_t k = 0; k < Order; ++k) {
                    z += w[k] * x(k);
                }
                for (size_t j = 0; j < ArOrder; ++j) {
                    z += w[Order + j] * r_p(j + 1);
                }
                state.a1[i] = std::tanh(z);
            }

            for (size_t i = 0; i < Hidden2; ++i) {
                Sample z = state.b2[i];
                const Sample* w = &state.W2[i * Hidden1];
                for (size_t k = 0; k < Hidden1; ++k) {
                    z += w[k] * state.a1[k];
                }
                state.a2[i] = std::tanh(z);
            }

            Sample r_hat = 0.f;
            for (size_t i = 0; i < Hidden2; ++i) {
                r_hat += state.W3[i] * state.a2[i];
            }

            Sample y = y0 + r_hat;
            out.push(y);

            Sample real = x(0);
            Sample past_pred = r_p(0);
            Sample err = std::clamp<Sample::storage>(real - past_pred, -1.f, 1.f);

            for (size_t i = 0; i < Hidden2; ++i) {
                state.W3[i] += _mu * err * state.a2[i];
            }

            for (size_t i = 0; i < Hidden2; ++i) {
                Sample delta2 = (state.W3[i] * err) * (1.f - state.a2[i] * state.a2[i]);
                Sample* w2 = &state.W2[i * Hidden1];
                for (size_t k = 0; k < Hidden1; ++k) {
                    w2[k] += _mu * delta2 * state.a1[k];
                }
                state.b2[i] += _mu * delta2;
                state.a2[i] = delta2;
            }

            for (size_t k1 = 0; k1 < Hidden1; ++k1) {
                Sample sum = 0.f;
                for (size_t i = 0; i < Hidden2; ++i) {
                    sum += state.W2[i * Hidden1 + k1] * state.a2[i];
                }
                Sample delta1 = sum * (1.f - state.a1[k1] * state.a1[k1]);
                Sample* w1 = &state.W1[k1];
                for (size_t k = 0; k < Order; ++k) {
                    w1[k] += _mu * delta1 * x(k);
                }
                for (size_t j = 0; j < ArOrder; ++j) {
                    w1[Order + j] += _mu * delta1 * r_p(j + 1);
                }
                state.b1[k1] += _mu * delta1;
            }
        }
    };

    template<size_t LookAhead, size_t Order>
    class PolyResidualPredictor {
        Sample _mu;

        struct State {
            std::span<Sample> w;
        };

        template<typename Buf>
        static State& st(Buf b)
        {
            void* o = b.data();
            size_t s = b.size();
            return *reinterpret_cast<State*>(std::align(alignof(State), sizeof(State), o, s));
        }

    public:
        PolyResidualPredictor(Sample mu = 1e-5f) :
            _mu(mu)
        {
            static_assert(Order >= LookAhead, "window length must cover look-ahead");
        }

        static constexpr auto inputs()
        {
            return std::array { InputConfig { .history = Order - 1 } };
        }

        static constexpr auto outputs()
        {
            return std::array { OutputConfig { .history = LookAhead } };
        }

        template<typename A>
        void init_buffer(A& alloc) const
        {
            State& s = alloc.template new_object<State>();
            alloc.assign(s.w, alloc.template new_array<Sample>(2 * Order));
            alloc.fill_n(s.w, 0.f);
        }

        void tick(TickSampleContext<PolyResidualPredictor> const& ctx) const
        {
            State& s = st(ctx.buffer);
            auto& in = ctx.inputs[0];
            auto& out = ctx.outputs[0];

            auto x = [&](size_t k) { return in.get(k); };

            Sample y0 = x(0);
            Sample r_hat = 0.f;
            for (size_t k = 0; k < Order; ++k) {
                Sample xk = x(k);
                Sample* wk = &s.w[2 * k];
                r_hat += wk[0] * xk + wk[1] * xk * xk;
            }

            Sample y = y0 + r_hat;
            out.push(y);

            Sample real = x(0);
            Sample past_pred = out.get(LookAhead);
            Sample err = std::clamp<Sample::storage>(real - past_pred, -1.f, 1.f);

            Sample norm = 1e-6f;
            for (size_t k = 0; k < Order; ++k) {
                Sample xk = x(k);
                norm += xk * xk + (xk * xk) * (xk * xk);
            }

            Sample g = _mu * err / norm;
            for (size_t k = 0; k < Order; ++k) {
                Sample xk = x(k);
                Sample* wk = &s.w[2 * k];
                wk[0] += g * xk;
                wk[1] += g * xk * xk;
            }
        }
    };
}
