#pragma once

#include <intravenous/node/lifecycle.h>

namespace iv {
// An ordinary replayable multiplication node. GraphJit may place it privately
// after a public output; its signal is the only connected input in that use.
template<ChannelTypeId Channels, SampleStreamLayout Layout>
struct OutputGainNode {
    static constexpr auto replay_mode = TickReplayMode::input_union;

    static constexpr auto inputs()
    {
        return std::tuple{
            input<Sample, FFTBlock>("signal",
                {.channel_layout = {Channels, Layout}},
                {.channel_layout = {Channels, Layout}}),
            input<Sample>("scale", {.default_value = 1.0f}),
        };
    }
    static constexpr auto outputs()
    {
        return std::tuple{output<Sample, FFTBlock>("scaled",
            {.channel_layout = {Channels, Layout}},
            {.channel_layout = {Channels, Layout}})};
    }
    void constrain_ports(ConstrainPortsContext<OutputGainNode>& ctx) const
    {
        ctx.template input<"signal">().value_type() = ctx.template output<"scaled">().value_type();
        ctx.template input<"signal">().size() = ctx.template output<"scaled">().size();
        ctx.template input<"signal">().pace() = ctx.template output<"scaled">().pace();
        ctx.template input<"scale">().pace() = ctx.template output<"scaled">().pace();
    }
    void tick(TickSampleContext<OutputGainNode> const& ctx) const
    {
        auto const signal = ctx.template input<"signal">();
        auto const gain = ctx.template input<"scale">();
        auto const output = ctx.template output<"scaled">();
        auto process = [&]<class Value>() {
            auto const source = signal.template as<Value>();
            auto const target = output.template as<Value>();
            auto factor = gain.get();
            for (std::size_t channel = 0; channel < channel_count(source.channel_layout()); ++channel) {
                if constexpr (std::same_as<Value, Sample>)
                    target(channel) = source.get(0, channel) * factor;
                else {
                    auto from = source.get(0, channel);
                    auto to = target(channel);
                    for (std::size_t bin = 0; bin < from.size(); ++bin)
                        to[bin] = from[bin] * factor.value;
                }
            }
        };
        if (signal.template is<Sample>()) process.template operator()<Sample>();
        else process.template operator()<FFTBlock>();
    }
};
using OutputGain = OutputGainNode<ChannelTypeId::mono, SampleStreamLayout::planar>;
} // namespace iv

// Register concrete channel/layout variants through the same package path as
// other nodes, so LLVM sees and specializes their ordinary authored callbacks.
#define IV_REGISTER_OUTPUT_GAIN_CHANNEL(name, wire_name, ...) \
    using IvOutputGain_##name##_planar = ::iv::OutputGainNode<::iv::ChannelTypeId::name, ::iv::SampleStreamLayout::planar>; \
    using IvOutputGain_##name##_interleaved = ::iv::OutputGainNode<::iv::ChannelTypeId::name, ::iv::SampleStreamLayout::interleaved>; \
    IV_NODE("iv.builtin.output_gain." wire_name ".planar", IvOutputGain_##name##_planar); \
    IV_NODE("iv.builtin.output_gain." wire_name ".interleaved", IvOutputGain_##name##_interleaved);
#define IV_REGISTER_OUTPUT_GAIN_NODES() IV_CHANNEL_TYPES(IV_REGISTER_OUTPUT_GAIN_CHANNEL)

