#pragma once

#include "node_lifecycle.h"

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

        void tick(TickSampleContext<Broadcast> const& state) const
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
            std::span<Sample> samples;
        };

        auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        size_t max_block_size() const
        {
            return loop_block_size;
        }

        void declare(DeclarationContext<DetachWriterNode> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.samples, ctx.max_block_size());
            ctx.export_array(id, state.samples);
        }

        void initialize(InitializationContext<DetachWriterNode> const& ctx) const
        {
            auto& state = ctx.state();
            std::ranges::fill(state.samples, Sample{});
        }

        void tick_block(TickBlockContext<DetachWriterNode> const& ctx) const
        {
            auto& state = ctx.state();
            auto const& samples = state.samples;
            auto const n = samples.size();

            auto block = ctx.inputs[0].get_block(ctx.block_size);

            auto const* src1 = block.first.data();
            auto const n1 = block.first.size();

            auto const* src2 = block.second.data();
            auto const n2 = block.second.size();

            auto const total = ctx.block_size;
            auto const start = ctx.index & (n - 1);

            auto* dst1 = samples.data() + start;
            auto const d1 = std::min(total, n - start); // before ring wrap
            auto* dst2 = samples.data();

            auto const a = std::min(n1, d1); // first  -> dst1
            auto const b = n1 - a;           // first  -> dst2
            auto const c = d1 - a;           // second -> dst1
            auto const d = n2 - c;           // second -> dst2

            std::copy_n(src1,     a, dst1);
            std::copy_n(src1 + a, b, dst2);
            std::copy_n(src2,     c, dst1 + a);
            std::copy_n(src2 + c, d, dst2);
        }
    };

    struct DetachReaderNode {
        BufferId id;
        size_t loop_block_size = 1;

        struct State {
            std::span<Sample> samples;
        };

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        size_t max_block_size() const
        {
            return loop_block_size;
        }

        void declare(DeclarationContext<DetachReaderNode> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.import_array(id, state.samples);
        }

        void tick_block(TickBlockContext<DetachReaderNode> const& ctx) const
        {
            auto& state = ctx.state();
            auto const& samples = state.samples;
            auto const n = samples.size();

            auto block = ctx.outputs[0].get_block(ctx.block_size);

            auto* dst1 = block.first.data();
            auto const n1 = block.first.size();
            auto* dst2 = block.second.data();

            auto const total = ctx.block_size;
            auto const start = (ctx.index + n - loop_block_size) & (n - 1);

            auto const src1 = std::min(total, n - start); // until ring wrap
            auto const src2 = total - src1;               // after ring wrap

            auto const a = std::min(src1, n1); // src1 -> first
            auto const b = src1 - a;           // src1 -> second
            auto const c = n1 - a;             // src2 -> first
            auto const d = src2 - c;           // src2 -> second

            std::copy_n(samples.data() + start,     a, dst1);
            std::copy_n(samples.data() + start + a, b, dst2);
            std::copy_n(samples.data(),             c, dst1 + a);
            std::copy_n(samples.data() + c,         d, dst2 + b);
        }
    };

    struct DummySink {
        auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        void tick(TickSampleContext<DummySink> const&) const
        {}
    };
}
