#pragma once

#include "node.h"

#include <array>
#include <string>
#include <vector>

namespace iv {
    struct BufferId {
        size_t id;

        BufferId(size_t id): id(id) {}

        operator std::string() const {
            return "detach:" + std::to_string(id);
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
            Sample sample = state.inputs[0].get();
            for (auto& out : state.outputs) {
                out.push(sample);
            }
        }
    };

    struct DetachWriterNode {
        BufferId id;

        struct State {
            Sample* slot{};
        };

        constexpr auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        template<class Alloc>
        void init_buffer(Alloc& alloc, InitBufferContext& ctx) const
        {
            State& st = alloc.template new_object<State>();
            auto span = alloc.template new_array<Sample>(1);
            alloc.assign(alloc.at(span, 0), Sample{ 0 });
            ctx.register_tick_buffer(id, span);
            alloc.assign(st.slot, span.data());
        }

        void tick(TickState const& ts) const
        {
            auto& st = ts.get_state<State>();
            *st.slot = ts.inputs[0].get();
        }
    };

    struct DetachReaderNode {
        BufferId id;

        struct State {
            Sample* slot{};
        };

        constexpr auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        template<class Alloc>
        void init_buffer(Alloc& alloc, InitBufferContext& ctx) const
        {
            State& st = alloc.template new_object<State>();
            auto span = ctx.template use_tick_buffer<Sample>(id);
            alloc.assign(st.slot, span.data());
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
