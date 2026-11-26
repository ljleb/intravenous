#pragma once

#include <intravenous/node/lifecycle.h>

namespace iv {
// An ordinary replayable multiplication node. GraphJit may place it privately
// after a public output; its signal is the only connected input in that use.
template<ChannelTypeId Channels, SampleStreamLayout Layout>
struct OutputGainNode {
    static constexpr auto replay_mode = TickReplayMode::enabled;

    static constexpr auto inputs()
    {
        return std::tuple{
            input<Sample, FFTBlock>({"signal", "Signal"},
                {.channel_layout = {Channels, Layout}},
                {.channel_layout = {Channels, Layout}}),
            input<Sample>({"scale", "Scale"}, {.default_value = 1.0f}),
        };
    }
    static constexpr auto outputs()
    {
        return std::tuple{output<Sample, FFTBlock>({"scaled", "Scaled"},
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
    void propagate_forward_coverage(PropagateForwardCoverageContext<OutputGainNode>& ctx) const
    {
        ctx.template output<"scaled">().publish(
            ctx.template input<"signal">().coverage() | ctx.template input<"scale">().coverage());
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

// GraphJit inserts these types after package construction. Emit their
// compiler records from the package without publishing node definitions.
#define IV_INSTANTIATE_OUTPUT_GAIN_CHANNEL(name, ...) \
    template const iv::details::NodeCompilerRecord iv::details::node_compiler_record< \
        iv::OutputGainNode<iv::ChannelTypeId::name, iv::SampleStreamLayout::planar>>; \
    template const iv::details::NodeCompilerRecord iv::details::node_compiler_record< \
        iv::OutputGainNode<iv::ChannelTypeId::name, iv::SampleStreamLayout::interleaved>>;
#define IV_INSTANTIATE_OUTPUT_GAIN_NODES() IV_CHANNEL_TYPES(IV_INSTANTIATE_OUTPUT_GAIN_CHANNEL)

