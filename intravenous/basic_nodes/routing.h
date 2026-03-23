#pragma once

#include "node.h"

#include <array>
#include <vector>

namespace iv {
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
            Sample sample = state.inputs[0].get();
            for (auto& out : state.outputs) {
                out.push(sample);
            }
        }
    };

    struct DetachWriterNode {
        size_t id;

        struct State {
            Sample* slot{};
        };

        constexpr auto inputs() const
        {
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

        constexpr auto outputs() const
        {
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
}
