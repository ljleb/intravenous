#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>
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
            return std::array<OutputConfig, 1>{realtime_sample_output("out", {
                .channel_layout = ChannelLayout{
                    .channel_type = ChannelTypeTraits<ChannelType>::id,
                    .sample_layout = SampleStreamLayout::planar,
                },
            })};
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
            return std::array<InputConfig, 1>{realtime_sample_input("in", {
                .channel_layout = ChannelLayout{
                    .channel_type = ChannelTypeTraits<ChannelType>::id,
                    .sample_layout = SampleStreamLayout::planar,
                },
            })};
        }

        static constexpr auto outputs()
        {
            auto configs = std::array<OutputConfig, ChannelType::channel_count>{};
            for (size_t channel = 0; channel < ChannelType::channel_count; ++channel) {
                configs[channel].name = ChannelType::channel_names[channel];
            }
            return configs;
        }

        void tick(TickSampleContext<ChannelUnpack> const& ctx) const
        {
            for_each_channel_port<ChannelType>([&]<auto channel>() {
                ctx.outputs[port_index(channel)].push(
                    ctx.inputs[0].get(0, port_index(channel)));
            });
        }
    };

    struct BroadcastEvent {
        size_t _num_outputs;
        EventTypeId _type;

        constexpr explicit BroadcastEvent(size_t num_outputs, EventTypeId type) :
            _num_outputs(num_outputs),
            _type(type)
        {}

        constexpr auto inputs() const
        {
            return std::array { realtime_event_input({}, _type) };
        }

        constexpr auto outputs() const
        {
            return std::vector<OutputConfig>(
                _num_outputs, realtime_event_output({}, _type));
        }

        void tick_block(TickBlockContext<BroadcastEvent> const& ctx) const
        {
            auto events = ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
            for (auto& output : ctx.event_outputs) {
                output.push_block(events);
            }
        }
    };

    struct EventConcatenation {
        size_t _num_inputs;
        EventTypeId _type;

        struct State {
            std::span<size_t> cursors;
        };

        constexpr explicit EventConcatenation(size_t num_inputs, EventTypeId type) :
            _num_inputs(num_inputs),
            _type(type)
        {}

        constexpr auto inputs() const
        {
            return std::vector<InputConfig>(_num_inputs, realtime_event_input({}, _type));
        }

        constexpr auto outputs() const
        {
            return std::array { realtime_event_output({}, _type) };
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

    struct DummySink {
        static constexpr auto inputs()
        {
            return std::array<InputConfig, 1>{};
        }

        void tick(TickSampleContext<DummySink> const&) const
        {}
    };

    struct DummyEventSink {
        static constexpr auto inputs()
        {
            return std::array { realtime_event_input({}, EventTypeId::empty) };
        }

        void tick_block(TickBlockContext<DummyEventSink> const&) const
        {}
    };
}
