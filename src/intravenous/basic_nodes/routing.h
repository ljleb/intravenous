#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>

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
