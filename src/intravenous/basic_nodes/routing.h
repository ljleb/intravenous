#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>
#include <string>
#include <vector>

namespace iv {
    template<class ChannelType>
    class ChannelPack {
    public:
        static constexpr auto inputs()
        {
            return std::array<InputConfig, ChannelType::channel_count>{};
        }

        static constexpr auto outputs()
        {
            return std::array<OutputConfig, 1>{OutputConfig{
                .name = "out",
                .channel_layout = ChannelLayout{
                    .channel_type = ChannelTypeTraits<ChannelType>::id,
                    .sample_layout = SampleStreamLayout::planar,
                },
            }};
        }

        void tick_block(TickBlockContext<ChannelPack> const& ctx) const
        {
            for_each_channel_port<ChannelType>([&]<auto channel>() {
                ctx.template output<"out">()[channel].write_block(
                    ctx.inputs[port_index(channel)].get_block(ctx.block_size));
            });
        }
    };

    template<class ChannelType>
    class ChannelUnpack {
    public:
        static constexpr auto inputs()
        {
            return std::array<InputConfig, 1>{InputConfig{
                .name = "in",
                .channel_layout = ChannelLayout{
                    .channel_type = ChannelTypeTraits<ChannelType>::id,
                    .sample_layout = SampleStreamLayout::planar,
                },
            }};
        }

        static constexpr auto outputs()
        {
            return std::array<OutputConfig, ChannelType::channel_count>{};
        }

        void tick(TickSampleContext<ChannelUnpack> const& ctx) const
        {
            for_each_channel_port<ChannelType>([&]<auto channel>() {
                ctx.outputs[port_index(channel)].push(
                    ctx.inputs[0].get(0, port_index(channel)));
            });
        }
    };

    struct DetachArrayId {
        size_t id;

        DetachArrayId(size_t id): id(id) {}

        operator std::string() const {
            return "detach:" + std::to_string(id);
        }
    };

    template<size_t NumOutputs>
    class Broadcast {
    public:
        static_assert(NumOutputs >= 1, "Broadcast requires at least one output");

        static constexpr auto inputs()
        {
            return std::array<InputConfig, 1>{};
        }

        static constexpr auto outputs()
        {
            return std::array<OutputConfig, NumOutputs>{};
        }

        void tick(TickSampleContext<Broadcast<NumOutputs>> const& state) const
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
                output.push_block(events);
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

                ctx.event_outputs[0].push(selected_event);
                ++state.cursors[selected_input];
            }
        }
    };

    struct DetachWriterNode {
        DetachArrayId id;
        size_t loop_extra_latency = 1;

        struct State {
            std::span<Sample> samples;
        };

        static constexpr auto inputs()
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

        static constexpr auto outputs()
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
        static constexpr auto inputs()
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
