#include "predictors.h"

#include <algorithm>
#include <cmath>

namespace iv {
    void NlmsPredictor::tick(TickState const& ts) const
    {
        State& st = ts.get_state<State>();
        auto& in = ts.inputs[0];
        auto& out = ts.outputs[0];

        Sample y = 0.f;
        for (size_t k = 0; k < _order; ++k) {
            y += st.w[k] * in.get(k);
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
            st.w[k] = st.w[k] * _decay + g * in.get(k);
        }
    }

    void TanhResidualPredictor::tick(TickState const& ts) const
    {
        State& s = ts.get_state<State>();
        auto& in = ts.inputs[0];
        auto& out = ts.outputs[0];

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

    void TanhResidualAR2Predictor::tick(TickState const& ts) const
    {
        State& s = st(ts.buffer);
        auto& in = ts.inputs[0];
        auto& out = ts.outputs[0];

        auto x = [&](size_t k) { return in.get(k); };
        auto r_p = [&](size_t j) { return out.get(_L + j); };

        Sample y0 = x(0);

        for (size_t i = 0; i < _h1; ++i) {
            Sample z = s.b1[i];
            const Sample* w = &s.W1[i * (_p + _q)];
            for (size_t k = 0; k < _p; ++k) {
                z += w[k] * x(k);
            }
            for (size_t j = 0; j < _q; ++j) {
                z += w[_p + j] * r_p(j + 1);
            }
            s.a1[i] = std::tanh(z);
        }

        for (size_t i = 0; i < _h2; ++i) {
            Sample z = s.b2[i];
            const Sample* w = &s.W2[i * _h1];
            for (size_t k = 0; k < _h1; ++k) {
                z += w[k] * s.a1[k];
            }
            s.a2[i] = std::tanh(z);
        }

        Sample r_hat = 0.f;
        for (size_t i = 0; i < _h2; ++i) {
            r_hat += s.W3[i] * s.a2[i];
        }

        Sample y = y0 + r_hat;
        out.push(y);

        Sample real = x(0);
        Sample past_pred = r_p(0);
        Sample err = std::clamp<Sample::storage>(real - past_pred, -1.f, 1.f);

        for (size_t i = 0; i < _h2; ++i) {
            s.W3[i] += _mu * err * s.a2[i];
        }

        for (size_t i = 0; i < _h2; ++i) {
            Sample delta2 = (s.W3[i] * err) * (1.f - s.a2[i] * s.a2[i]);
            Sample* w2 = &s.W2[i * _h1];
            for (size_t k = 0; k < _h1; ++k) {
                w2[k] += _mu * delta2 * s.a1[k];
            }
            s.b2[i] += _mu * delta2;
            s.a2[i] = delta2;
        }

        for (size_t k1 = 0; k1 < _h1; ++k1) {
            Sample sum = 0.f;
            for (size_t i = 0; i < _h2; ++i) {
                sum += s.W2[i * _h1 + k1] * s.a2[i];
            }
            Sample delta1 = sum * (1.f - s.a1[k1] * s.a1[k1]);
            Sample* w1 = &s.W1[k1];
            for (size_t k = 0; k < _p; ++k) {
                w1[k] += _mu * delta1 * x(k);
            }
            for (size_t j = 0; j < _q; ++j) {
                w1[_p + j] += _mu * delta1 * r_p(j + 1);
            }
            s.b1[k1] += _mu * delta1;
        }
    }

    void PolyResidualPredictor::tick(TickState const& ts) const
    {
        State& s = st(ts.buffer);
        auto& in = ts.inputs[0];
        auto& out = ts.outputs[0];

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
}
