#pragma once

#include "node_lifecycle.h"

#include <array>
#include <string>
#include <vector>

namespace iv {
    struct DetachArrayId {
        size_t id;

        DetachArrayId(size_t id): id(id) {}

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

    class BroadcastEvent {
        size_t _num_outputs;

    public:
        explicit BroadcastEvent(size_t num_outputs) :
            _num_outputs(num_outputs)
        {}

        auto event_inputs() const
        {
            return std::array<EventInputConfig, 1>{};
        }

        auto event_outputs() const
        {
            return std::vector<EventOutputConfig>(_num_outputs);
        }

        auto num_event_outputs() const
        {
            return _num_outputs;
        }

        void tick_block(TickBlockContext<BroadcastEvent> const& ctx) const
        {
            auto events = ctx.event_inputs[0].get_block(ctx.event_stream_storage(), ctx.index, ctx.block_size);
            for (auto& output : ctx.event_outputs) {
                output.push_block(ctx.event_stream_storage(), events, ctx.index, ctx.block_size);
            }
        }
    };

    class EventConcatenation {
        size_t _num_inputs;

    public:
        explicit EventConcatenation(size_t num_inputs) :
            _num_inputs(num_inputs)
        {}

        auto event_inputs() const
        {
            return std::vector<EventInputConfig>(_num_inputs);
        }

        auto event_outputs() const
        {
            return std::array<EventOutputConfig, 1>{};
        }

        auto num_event_inputs() const
        {
            return _num_inputs;
        }

        void tick_block(TickBlockContext<EventConcatenation> const& ctx) const
        {
            for (auto const& input : ctx.event_inputs) {
                input.for_each_in_block(ctx.event_stream_storage(), ctx.index, ctx.block_size, [&](TimedEvent const& event, size_t) {
                    ctx.event_outputs[0].push(ctx.event_stream_storage(), event.value, event.time, ctx.index, ctx.block_size);
                });
            }
        }
    };

    struct DetachWriterNode {
        DetachArrayId id;
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
            auto const& src = ctx.inputs[0].get_block(ctx.block_size);
            auto const& dst = make_block_view(state.samples, ctx.index & (state.samples.size() - 1), ctx.block_size);
            src.copy_to(dst);
        }
    };

    struct DetachReaderNode {
        DetachArrayId id;
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

            auto const total = ctx.block_size;
            auto const start = (ctx.index + n - loop_block_size) & (n - 1);
            BlockView<Sample const> const samples_block {
                std::span<Sample const>(samples.data() + start, std::min(total, n - start)),
                std::span<Sample const>(samples.data(), total - std::min(total, n - start)),
            };
            ctx.outputs[0].push_block(samples_block);
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

    struct DummyEventSink {
        auto event_inputs() const
        {
            return std::array<EventInputConfig, 1>{};
        }

        void tick_block(TickBlockContext<DummyEventSink> const&) const
        {}
    };
}
