#include "predictors.h"

#include <algorithm>
#include <cmath>

namespace iv {
    void NlmsPredictor::tick(TickState const& ts) const
    {
        State& st = ts.get_state<State>();
        auto& in = ts.inputs[0];
        auto& out = ts.outputs[0];

        float y = 0.f;
        for (size_t k = 0; k < _order; ++k) {
            y += st.w[k] * in.get(k);
        }
        out.push(y);

        float real = in.get(0);
        float past_pred = out.get(_look_ahead);
        float err = real - past_pred;

        float norm = 1e-6f;
        for (size_t k = 0; k < _order; ++k) {
            float s = in.get(k);
            norm += s * s;
        }

        float g = _lr * err / norm;
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

        float y0 = x(0);

        for (size_t i = 0; i < _h; ++i) {
            float z = s.b1[i];
            const float* w = &s.W1[i * (_p + _q)];
            for (size_t k = 0; k < _p; ++k) {
                z += w[k] * x(k);
            }
            for (size_t j = 1; j <= _q; ++j) {
                z += w[_p + (j - 1)] * r_prev(j);
            }
            s.a[i] = std::tanh(z);
        }

        float r_hat = 0.f;
        for (size_t i = 0; i < _h; ++i) {
            r_hat += s.W2[i] * s.a[i];
        }

        float y = y0 + r_hat;
        out.push(y);

        float real = in.get(0);
        float past_pred = r_prev(0);
        float err = real - past_pred;

        for (size_t i = 0; i < _h; ++i) {
            s.W2[i] += _mu * err * s.a[i];
        }

        for (size_t i = 0; i < _h; ++i) {
            float delta = (s.W2[i] * err) * (1.f - s.a[i] * s.a[i]);
            float* w = &s.W1[i * (_p + _q)];
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

        float y0 = x(0);

        for (size_t i = 0; i < _h1; ++i) {
            float z = s.b1[i];
            const float* w = &s.W1[i * (_p + _q)];
            for (size_t k = 0; k < _p; ++k) {
                z += w[k] * x(k);
            }
            for (size_t j = 0; j < _q; ++j) {
                z += w[_p + j] * r_p(j + 1);
            }
            s.a1[i] = std::tanh(z);
        }

        for (size_t i = 0; i < _h2; ++i) {
            float z = s.b2[i];
            const float* w = &s.W2[i * _h1];
            for (size_t k = 0; k < _h1; ++k) {
                z += w[k] * s.a1[k];
            }
            s.a2[i] = std::tanh(z);
        }

        float r_hat = 0.f;
        for (size_t i = 0; i < _h2; ++i) {
            r_hat += s.W3[i] * s.a2[i];
        }

        float y = y0 + r_hat;
        out.push(y);

        float real = x(0);
        float past_pred = r_p(0);
        float err = std::clamp(real - past_pred, -1.f, 1.f);

        for (size_t i = 0; i < _h2; ++i) {
            s.W3[i] += _mu * err * s.a2[i];
        }

        for (size_t i = 0; i < _h2; ++i) {
            float delta2 = (s.W3[i] * err) * (1.f - s.a2[i] * s.a2[i]);
            float* w2 = &s.W2[i * _h1];
            for (size_t k = 0; k < _h1; ++k) {
                w2[k] += _mu * delta2 * s.a1[k];
            }
            s.b2[i] += _mu * delta2;
            s.a2[i] = delta2;
        }

        for (size_t k1 = 0; k1 < _h1; ++k1) {
            float sum = 0.f;
            for (size_t i = 0; i < _h2; ++i) {
                sum += s.W2[i * _h1 + k1] * s.a2[i];
            }
            float delta1 = sum * (1.f - s.a1[k1] * s.a1[k1]);
            float* w1 = &s.W1[k1];
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

        float y0 = x(0);
        float r_hat = 0.f;
        for (size_t k = 0; k < _p; ++k) {
            float xk = x(k);
            float* wk = &s.w[2 * k];
            r_hat += wk[0] * xk + wk[1] * xk * xk;
        }

        float y = y0 + r_hat;
        out.push(y);

        float real = x(0);
        float past_pred = out.get(_L);
        float err = std::clamp(real - past_pred, -1.f, 1.f);

        float norm = 1e-6f;
        for (size_t k = 0; k < _p; ++k) {
            float xk = x(k);
            norm += xk * xk + (xk * xk) * (xk * xk);
        }

        float g = _mu * err / norm;
        for (size_t k = 0; k < _p; ++k) {
            float xk = x(k);
            float* wk = &s.w[2 * k];
            wk[0] += g * xk;
            wk[1] += g * xk * xk;
        }
    }
}
