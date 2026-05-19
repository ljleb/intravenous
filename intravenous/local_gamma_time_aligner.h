#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace iv {
    class LocalGammaTimeAligner {
    public:
        void reset()
        {
            _initialized = false;
            _last_timestamp = 0.0;
            _last_sample_index = 0;
            _beta = 0.0;
            _sigma2 = kInitialSigma * kInitialSigma;
        }

        bool initialized() const
        {
            return _initialized;
        }

        void observe_callback(double timestamp, size_t sample_index, double dt)
        {
            if (!_initialized) {
                _initialized = true;
                _last_timestamp = timestamp;
                _last_sample_index = sample_index;
                _beta = 0.0;
                _sigma2 = kInitialSigma * kInitialSigma;
                return;
            }

            double const callback_delta = timestamp - _last_timestamp;
            if (callback_delta <= 0.0 || sample_index < _last_sample_index) {
                reset();
                observe_callback(timestamp, sample_index, dt);
                return;
            }

            size_t const sample_delta = sample_index - _last_sample_index;
            double const x = callback_delta / dt;
            double const m = static_cast<double>(sample_delta) + _beta;
            double const r = x - m;

            _sigma2 = (1.0 - kLambdaSigma) * _sigma2 + kLambdaSigma * r * r;
            double const sigma = std::sqrt(std::max(_sigma2, kEps));
            double const lambda_beta = gamma_weight(x, m, sigma);

            _beta = (m - x) + lambda_beta * r;
            _last_timestamp = timestamp;
            _last_sample_index = sample_index;
        }

        double predict_sample_offset(double timestamp, double dt) const
        {
            return ((timestamp - _last_timestamp) / dt) - _beta;
        }

        double beta() const
        {
            return _beta;
        }

        double sigma2() const
        {
            return _sigma2;
        }

    private:
        static double gamma_weight(double x, double m, double sigma)
        {
            if (x <= 0.0) {
                return 0.0;
            }

            double const s = sigma + kEps;
            if (m <= kEps) {
                return std::clamp(std::exp(-x / s), 0.0, 1.0);
            }

            double log_weight = (m / s) * std::log((x + kEps) / (m + kEps)) - (x - m) / s;
            log_weight = std::min(log_weight, 0.0);
            return std::clamp(std::exp(log_weight), 0.0, 1.0);
        }

        static constexpr double kInitialSigma = 1.0;
        static constexpr double kLambdaSigma = 0.01;
        static constexpr double kEps = 1.0e-12;

        bool _initialized = false;
        double _last_timestamp = 0.0;
        size_t _last_sample_index = 0;
        double _beta = 0.0;
        double _sigma2 = kInitialSigma * kInitialSigma;
    };
}
