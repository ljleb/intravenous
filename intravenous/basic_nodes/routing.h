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
        explicit Broadcast(size_t num_outputs) :
            _num_outputs(num_outputs)
        {}

        auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        auto outputs() const
        {
            return std::vector<OutputConfig>(_num_outputs);
        }

        auto num_outputs() const
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
        size_t loop_block_size = 1;

        struct State {
            std::span<Sample> slot;
        };

        auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        template<class Alloc>
        void init_buffer(Alloc& alloc, InitBufferContext& ctx) const
        {
            State& st = alloc.template new_object<State>();
            auto span = alloc.template new_array<Sample>(ctx.max_block_size);
            alloc.fill_n(span, Sample{ 0 });
            ctx.register_tick_buffer(id, span);
            alloc.assign(st.slot, span);
        }

        size_t max_block_size() const
        {
            return loop_block_size;
        }

        void tick(TickState const& ts) const
        {
            auto& st = ts.get_state<State>();
            size_t const slot_index = ts.index & (st.slot.size() - 1);
            Sample const value = ts.inputs[0].get();
            st.slot[slot_index] = value;
        }

        void tick_block(BlockTickState const& ts) const
        {
            auto& st = ts.get_state<State>();
            auto block = ts.inputs[0].get_block(ts.block_size);
            for (size_t sample = 0; sample < ts.block_size; ++sample) {
                st.slot[(ts.index + sample) & (st.slot.size() - 1)] = block[sample];
            }
        }
    };

    struct DetachReaderNode {
        BufferId id;
        size_t loop_block_size = 1;

        struct State {
            std::span<Sample> slot;
        };

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        template<class Alloc>
        void init_buffer(Alloc& alloc, InitBufferContext& ctx) const
        {
            State& st = alloc.template new_object<State>();
            auto span = ctx.template use_tick_buffer<Sample>(id);
            alloc.assign(st.slot, span);
        }

        size_t max_block_size() const
        {
            return loop_block_size;
        }

        void tick(TickState const& ts) const
        {
            auto& st = ts.get_state<State>();
            size_t const slot_index = (ts.index + st.slot.size() - loop_block_size) & (st.slot.size() - 1);
            Sample const value = st.slot[slot_index];
            ts.outputs[0].push(value);
        }

        void tick_block(BlockTickState const& ts) const
        {
            auto& st = ts.get_state<State>();
            std::vector<Sample> block(ts.block_size);
            for (size_t sample = 0; sample < ts.block_size; ++sample) {
                block[sample] = st.slot[
                    (ts.index + sample + st.slot.size() - loop_block_size) & (st.slot.size() - 1)
                ];
            }
            ts.outputs[0].push_block(block);
        }
    };

    struct DummySink {
        auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        void tick(TickState const&) const
        {}
    };
}
