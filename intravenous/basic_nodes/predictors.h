#pragma once

#include "node/lifecycle.h"

#include <ranges>
#include <array>
#include <cassert>

namespace iv {
    class NlmsPredictor {
        size_t _look_ahead;
        size_t _order;
        Sample _lr;
        Sample _decay;

    public:
        struct State {
            std::span<Sample> w;
        };

        NlmsPredictor(size_t look_ahead, size_t order, Sample lr = 1e-4, Sample decay = 1.0) :
            _look_ahead(look_ahead),
            _order(order),
            _lr(lr),
            _decay(decay)
        {
            IV_ASSERT(_order >= _look_ahead, "window length must cover look-ahead");
        }

        auto inputs() const
        {
            return std::array { InputConfig { .history = _order - 1 } };
        }

        auto outputs() const
        {
            return std::array { OutputConfig { .history = _look_ahead } };
        }

        void declare(DeclarationContext<NlmsPredictor> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.w, _order);
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
            for (size_t k = 0; k < _order; ++k) {
                y += state.w[k] * in.get(k);
            }
            out.push(y);

            Sample real = in.get(0);
            Sample past_pred = out.get(_look_ahead);
            Sample err = real - past_pred;

            Sample norm = 1e-6f;
            for (size_t k = 0; k < _order; ++k) {
                Sample s = in.get(k);
                norm += s * s;
            }

            Sample g = _lr * err / norm;
            for (size_t k = 0; k < _order; ++k) {
                state.w[k] = state.w[k] * _decay + g * in.get(k);
            }
        }
    };

    class TanhResidualPredictor {
        size_t _L;
        size_t _p;
        size_t _q;
        size_t _h;
        Sample _mu;

    public:
        struct State {
            std::span<Sample> W1;
            std::span<Sample> W2;
            std::span<Sample> b1;
            std::span<Sample> a;
        };

        TanhResidualPredictor(
            size_t look_ahead,
            size_t order,
            size_t ar_order = 2,
            size_t hidden = 8,
            Sample mu = 1e-6f
        ) :
            _L(look_ahead),
            _p(order),
            _q(ar_order),
            _h(hidden),
            _mu(mu)
        {
            IV_ASSERT(_p >= _L, "window length must cover look-ahead");
        }

        auto inputs() const
        {
            return std::array { InputConfig { .history = _p - 1 } };
        }

        auto outputs() const
        {
            return std::array { OutputConfig { .history = _L + _q } };
        }

        void declare(DeclarationContext<TanhResidualPredictor> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.W1, _h * (_p + _q));
            ctx.local_array(state.W2, _h);
            ctx.local_array(state.b1, _h);
            ctx.local_array(state.a, _h);
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
            auto r_prev = [&](size_t j) { return out.get(_L + j); };

            Sample y0 = x(0);

            for (size_t i = 0; i < _h; ++i) {
                Sample z = s.b1[i];
                const Sample* w = &s.W1[i * (_p + _q)];
                for (size_t k = 0; k < _p; ++k) {
                    z += w[k] * x(k);
                }
                for (size_t j = 1; j <= _q; ++j) {
                    z += w[_p + (j - 1)] * r_prev(j);
                }
                s.a[i] = std::tanh(z);
            }

            Sample r_hat = 0.f;
            for (size_t i = 0; i < _h; ++i) {
                r_hat += s.W2[i] * s.a[i];
            }

            Sample y = y0 + r_hat;
            out.push(y);

            Sample real = in.get(0);
            Sample past_pred = r_prev(0);
            Sample err = real - past_pred;

            for (size_t i = 0; i < _h; ++i) {
                s.W2[i] += _mu * err * s.a[i];
            }

            for (size_t i = 0; i < _h; ++i) {
                Sample delta = (s.W2[i] * err) * (1.f - s.a[i] * s.a[i]);
                Sample* w = &s.W1[i * (_p + _q)];
                for (size_t k = 0; k < _p; ++k) {
                    w[k] += _mu * delta * x(k);
                }
                for (size_t j = 1; j <= _q; ++j) {
                    w[_p + (j - 1)] += _mu * delta * r_prev(j);
                }
                s.b1[i] += _mu * delta;
            }
        }
    };

    class TanhResidualAR2Predictor {
        size_t _L;
        size_t _p;
        size_t _q;
        size_t _h1;
        size_t _h2;
        Sample _mu;

    public:
        struct State {
            std::span<Sample> W1, b1;
            std::span<Sample> W2, b2;
            std::span<Sample> W3;
            std::span<Sample> a1, a2;
        };

        TanhResidualAR2Predictor(
            size_t look_ahead,
            size_t order,
            size_t ar_order = 2,
            size_t hidden1 = 16,
            size_t hidden2 = 8,
            Sample mu = 2e-6f
        )
        : _L(look_ahead)
        , _p(order)
        , _q(ar_order)
        , _h1(hidden1)
        , _h2(hidden2)
        , _mu(mu)
        {
            IV_ASSERT(_p >= _L, "window length must cover look-ahead");
        }

        auto inputs() const
        {
            return std::array { InputConfig { .history = _p - 1 } };
        }

        auto outputs() const
        {
            return std::array { OutputConfig { .latency = 0, .history = _L + _q } };
        }

        void declare(DeclarationContext<TanhResidualAR2Predictor> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.W1, _h1 * (_p + _q));
            ctx.local_array(state.b1, _h1);
            ctx.local_array(state.W2, _h2 * _h1);
            ctx.local_array(state.b2, _h2);
            ctx.local_array(state.W3, _h2);
            ctx.local_array(state.a1, _h1);
            ctx.local_array(state.a2, _h2);
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
            auto r_p = [&](size_t j) { return out.get(_L + j); };

            Sample y0 = x(0);

            for (size_t i = 0; i < _h1; ++i) {
                Sample z = state.b1[i];
                const Sample* w = &state.W1[i * (_p + _q)];
                for (size_t k = 0; k < _p; ++k) {
                    z += w[k] * x(k);
                }
                for (size_t j = 0; j < _q; ++j) {
                    z += w[_p + j] * r_p(j + 1);
                }
                state.a1[i] = std::tanh(z);
            }

            for (size_t i = 0; i < _h2; ++i) {
                Sample z = state.b2[i];
                const Sample* w = &state.W2[i * _h1];
                for (size_t k = 0; k < _h1; ++k) {
                    z += w[k] * state.a1[k];
                }
                state.a2[i] = std::tanh(z);
            }

            Sample r_hat = 0.f;
            for (size_t i = 0; i < _h2; ++i) {
                r_hat += state.W3[i] * state.a2[i];
            }

            Sample y = y0 + r_hat;
            out.push(y);

            Sample real = x(0);
            Sample past_pred = r_p(0);
            Sample err = std::clamp<Sample::storage>(real - past_pred, -1.f, 1.f);

            for (size_t i = 0; i < _h2; ++i) {
                state.W3[i] += _mu * err * state.a2[i];
            }

            for (size_t i = 0; i < _h2; ++i) {
                Sample delta2 = (state.W3[i] * err) * (1.f - state.a2[i] * state.a2[i]);
                Sample* w2 = &state.W2[i * _h1];
                for (size_t k = 0; k < _h1; ++k) {
                    w2[k] += _mu * delta2 * state.a1[k];
                }
                state.b2[i] += _mu * delta2;
                state.a2[i] = delta2;
            }

            for (size_t k1 = 0; k1 < _h1; ++k1) {
                Sample sum = 0.f;
                for (size_t i = 0; i < _h2; ++i) {
                    sum += state.W2[i * _h1 + k1] * state.a2[i];
                }
                Sample delta1 = sum * (1.f - state.a1[k1] * state.a1[k1]);
                Sample* w1 = &state.W1[k1];
                for (size_t k = 0; k < _p; ++k) {
                    w1[k] += _mu * delta1 * x(k);
                }
                for (size_t j = 0; j < _q; ++j) {
                    w1[_p + j] += _mu * delta1 * r_p(j + 1);
                }
                state.b1[k1] += _mu * delta1;
            }
        }
    };

    class PolyResidualPredictor {
        size_t _L;
        size_t _p;
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
        PolyResidualPredictor(size_t look_ahead, size_t order, Sample mu = 1e-5f) :
            _L(look_ahead),
            _p(order),
            _mu(mu)
        {
            IV_ASSERT(_p >= _L, "window length must cover look-ahead");
        }

        auto inputs() const
        {
            return std::array { InputConfig { .history = _p - 1 } };
        }

        auto outputs() const
        {
            return std::array { OutputConfig { .history = _L } };
        }

        template<typename A>
        void init_buffer(A& alloc) const
        {
            State& s = alloc.template new_object<State>();
            alloc.assign(s.w, alloc.template new_array<Sample>(2 * _p));
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
            for (size_t k = 0; k < _p; ++k) {
                Sample xk = x(k);
                Sample* wk = &s.w[2 * k];
                r_hat += wk[0] * xk + wk[1] * xk * xk;
            }

            Sample y = y0 + r_hat;
            out.push(y);

            Sample real = x(0);
            Sample past_pred = out.get(_L);
            Sample err = std::clamp<Sample::storage>(real - past_pred, -1.f, 1.f);

            Sample norm = 1e-6f;
            for (size_t k = 0; k < _p; ++k) {
                Sample xk = x(k);
                norm += xk * xk + (xk * xk) * (xk * xk);
            }

            Sample g = _mu * err / norm;
            for (size_t k = 0; k < _p; ++k) {
                Sample xk = x(k);
                Sample* wk = &s.w[2 * k];
                wk[0] += g * xk;
                wk[1] += g * xk * xk;
            }
        }
    };
}
