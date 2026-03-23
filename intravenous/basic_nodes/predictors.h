#pragma once

#include "node.h"

#include <array>
#include <cassert>

namespace iv {
    class NlmsPredictor {
        size_t _look_ahead;
        size_t _order;
        float _lr;
        float _decay;

        struct State {
            std::span<Sample> w;
        };

    public:
        constexpr NlmsPredictor(size_t look_ahead, size_t order, float lr = 1e-4, float decay = 1.0) :
            _look_ahead(look_ahead),
            _order(order),
            _lr(lr),
            _decay(decay)
        {
            assert(_order >= _look_ahead && "window length must cover look-ahead");
        }

        constexpr auto inputs() const
        {
            return std::array { InputConfig { .history = _order - 1 } };
        }

        constexpr auto outputs() const
        {
            return std::array { OutputConfig { .history = _look_ahead } };
        }

        template<typename Alloc>
        void init_buffer(Alloc& alloc) const
        {
            State& st = alloc.template new_object<State>();
            alloc.assign(st.w, alloc.template new_array<float>(_order));
            alloc.fill_n(st.w, 0.f);
            alloc.assign(alloc.at(st.w, 0), 1.f);
        }

        void tick(TickState const& ts) const;
    };

    class TanhResidualPredictor {
        size_t _L;
        size_t _p;
        size_t _q;
        size_t _h;
        float _mu;

        struct State {
            std::span<float> W1;
            std::span<float> W2;
            std::span<float> b1;
            std::span<float> a;
        };

    public:
        constexpr TanhResidualPredictor(
            size_t look_ahead,
            size_t order,
            size_t ar_order = 2,
            size_t hidden = 8,
            float mu = 1e-6f
        ) :
            _L(look_ahead),
            _p(order),
            _q(ar_order),
            _h(hidden),
            _mu(mu)
        {
            assert(_p >= _L && "window length must cover look-ahead");
        }

        constexpr auto inputs() const
        {
            return std::array { InputConfig { .history = _p - 1 } };
        }

        constexpr auto outputs() const
        {
            return std::array { OutputConfig { .history = _L + _q } };
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

        void tick(TickState const& ts) const;
    };

    class TanhResidualAR2Predictor {
        size_t _L;
        size_t _p;
        size_t _q;
        size_t _h1;
        size_t _h2;
        float _mu;

        struct State {
            std::span<float> W1, b1;
            std::span<float> W2, b2;
            std::span<float> W3;
            std::span<float> a1, a2;
        };

        template<typename Buf>
        static State& st(Buf b)
        {
            void* o = b.data();
            size_t s = b.size();
            return *reinterpret_cast<State*>(std::align(alignof(State), sizeof(State), o, s));
        }

    public:
        constexpr TanhResidualAR2Predictor(
            size_t look_ahead,
            size_t order,
            size_t ar_order = 2,
            size_t hidden1 = 16,
            size_t hidden2 = 8,
            float mu = 2e-6f
        ) :
            _L(look_ahead),
            _p(order),
            _q(ar_order),
            _h1(hidden1),
            _h2(hidden2),
            _mu(mu)
        {
            assert(_p >= _L);
        }

        constexpr auto inputs() const
        {
            return std::array { InputConfig { .history = _p - 1 } };
        }

        constexpr auto outputs() const
        {
            return std::array { OutputConfig { .latency = 0, .history = _L + _q } };
        }

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
            alloc.fill_n(s.W1, 0.f);
            alloc.fill_n(s.W2, 0.f);
            alloc.fill_n(s.W3, 0.f);
            alloc.fill_n(s.b1, 0.f);
            alloc.fill_n(s.b2, 0.f);
        }

        void tick(TickState const& ts) const;
    };

    class PolyResidualPredictor {
        size_t _L;
        size_t _p;
        float _mu;

        struct State {
            std::span<float> w;
        };

        template<typename Buf>
        static State& st(Buf b)
        {
            void* o = b.data();
            size_t s = b.size();
            return *reinterpret_cast<State*>(std::align(alignof(State), sizeof(State), o, s));
        }

    public:
        constexpr PolyResidualPredictor(size_t look_ahead, size_t order, float mu = 1e-5f) :
            _L(look_ahead),
            _p(order),
            _mu(mu)
        {
            assert(_p >= _L);
        }

        constexpr auto inputs() const
        {
            return std::array { InputConfig { .history = _p - 1 } };
        }

        constexpr auto outputs() const
        {
            return std::array { OutputConfig { .history = _L } };
        }

        template<typename A>
        void init_buffer(A& alloc) const
        {
            State& s = alloc.template new_object<State>();
            alloc.assign(s.w, alloc.template new_array<float>(2 * _p));
            alloc.fill_n(s.w, 0.f);
        }

        void tick(TickState const& ts) const;
    };
}
