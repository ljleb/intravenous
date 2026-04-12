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
        EventTypeId _type;

    public:
        explicit BroadcastEvent(size_t num_outputs, EventTypeId type) :
            _num_outputs(num_outputs),
            _type(type)
        {}

        auto event_inputs() const
        {
            return std::array<EventInputConfig, 1> {{
                { .type = _type }
            }};
        }

        auto event_outputs() const
        {
            return std::vector<EventOutputConfig>(_num_outputs, EventOutputConfig{ .type = _type });
        }

        auto num_event_outputs() const
        {
            return _num_outputs;
        }

        void tick_block(TickBlockContext<BroadcastEvent> const& ctx) const
        {
            auto events = ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
            for (auto& output : ctx.event_outputs) {
                output.push_block(events, ctx.index, ctx.block_size);
            }
        }
    };

    class EventConcatenation {
        size_t _num_inputs;
        EventTypeId _type;

    public:
        struct State {
            std::span<size_t> cursors;
        };

        explicit EventConcatenation(size_t num_inputs, EventTypeId type) :
            _num_inputs(num_inputs),
            _type(type)
        {}

        auto event_inputs() const
        {
            return std::vector<EventInputConfig>(_num_inputs, EventInputConfig{ .type = _type });
        }

        auto event_outputs() const
        {
            return std::array<EventOutputConfig, 1> {{
                { .type = _type }
            }};
        }

        auto num_event_inputs() const
        {
            return _num_inputs;
        }

        void declare(DeclarationContext<EventConcatenation> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.cursors, _num_inputs);
        }

        void tick_block(TickBlockContext<EventConcatenation> const& ctx) const
        {
            auto& state = ctx.state();
            auto const num_inputs = ctx.event_inputs.size();
            std::ranges::fill(state.cursors, 0);

            while (true) {
                size_t selected_input = num_inputs;
                TimedEvent selected_event {};
                for (size_t input_i = 0; input_i < num_inputs; ++input_i) {
                    auto const block = ctx.event_inputs[input_i].get_block(ctx.index, ctx.block_size);
                    if (state.cursors[input_i] >= block.size()) {
                        continue;
                    }
                    TimedEvent const event = block[state.cursors[input_i]];
                    if (selected_input == num_inputs || event.time < selected_event.time) {
                        selected_input = input_i;
                        selected_event = event;
                    }
                }

                if (selected_input == num_inputs) {
                    break;
                }

                ctx.event_outputs[0].push(selected_event.value, selected_event.time, ctx.index, ctx.block_size);
                ++state.cursors[selected_input];
            }
        }
    };

    class PolyphonicMix {
        size_t _arity;
        std::vector<OutputConfig> _outputs;
        std::vector<EventOutputConfig> _event_outputs;

    private:
        size_t sample_input_index(size_t output_port, size_t lane) const
        {
            return output_port * _arity + lane;
        }

        size_t event_input_index(size_t output_port, size_t lane) const
        {
            return output_port * _arity + lane;
        }

    public:
        struct State {
            std::span<size_t> cursors;
        };

        PolyphonicMix(
            size_t arity,
            std::vector<OutputConfig> outputs,
            std::vector<EventOutputConfig> event_outputs
        ) :
            _arity(arity),
            _outputs(std::move(outputs)),
            _event_outputs(std::move(event_outputs))
        {}

        auto inputs() const
        {
            return std::vector<InputConfig>(_outputs.size() * _arity);
        }

        auto event_inputs() const
        {
            std::vector<EventInputConfig> inputs;
            inputs.reserve(_event_outputs.size() * _arity);
            for (auto const& output : _event_outputs) {
                for (size_t lane = 0; lane < _arity; ++lane) {
                    (void)lane;
                    inputs.push_back(EventInputConfig{
                        .type = output.type,
                    });
                }
            }
            return inputs;
        }

        std::vector<OutputConfig> const& outputs() const
        {
            return _outputs;
        }

        std::vector<EventOutputConfig> const& event_outputs() const
        {
            return _event_outputs;
        }

        void declare(DeclarationContext<PolyphonicMix> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.cursors, _arity);
        }

        void tick_block(TickBlockContext<PolyphonicMix> const& ctx) const
        {
            for (size_t output_port = 0; output_port < _outputs.size(); ++output_port) {
                auto& output = ctx.outputs[output_port];
                output.push_silence(ctx.block_size);
                for (size_t lane = 0; lane < _arity; ++lane) {
                    output.accumulate_block(ctx.inputs[sample_input_index(output_port, lane)].get_block(ctx.block_size));
                }
            }

            auto& state = ctx.state();
            for (size_t output_port = 0; output_port < _event_outputs.size(); ++output_port) {
                std::ranges::fill(state.cursors, 0);

                while (true) {
                    size_t selected_lane = _arity;
                    TimedEvent selected_event {};
                    for (size_t lane = 0; lane < _arity; ++lane) {
                        auto const block = ctx.event_inputs[event_input_index(output_port, lane)].get_block(ctx.index, ctx.block_size);
                        if (state.cursors[lane] >= block.size()) {
                            continue;
                        }
                        TimedEvent const event = block[state.cursors[lane]];
                        if (selected_lane == _arity || event.time < selected_event.time) {
                            selected_lane = lane;
                            selected_event = event;
                        }
                    }

                    if (selected_lane == _arity) {
                        break;
                    }

                    ctx.event_outputs[output_port].push(selected_event.value, selected_event.time, ctx.index, ctx.block_size);
                    ++state.cursors[selected_lane];
                }
            }
        }
    };

    struct DetachWriterNode {
        DetachArrayId id;
        size_t loop_extra_latency = 1;

        struct State {
            std::span<Sample> samples;
        };

        auto inputs() const
        {
            return std::array<InputConfig, 1>{};
        }

        void declare(DeclarationContext<DetachWriterNode> const& ctx) const
        {
            auto const& state = ctx.state();
            size_t const min_size = loop_extra_latency + ctx.max_block_size();
            ctx.local_array(state.samples, next_power_of_2(min_size));
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
        size_t loop_extra_latency = 1;

        struct State {
            std::span<Sample> samples;
        };

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
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
            auto const start = (ctx.index + n - loop_extra_latency) & (n - 1);
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
            return std::array<EventInputConfig, 1> {{
                { .type = EventTypeId::empty }
            }};
        }

        void tick_block(TickBlockContext<DummyEventSink> const&) const
        {}
    };
}
