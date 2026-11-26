#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>
#include <cmath>
#include <optional>
#include <tuple>

namespace iv {

struct TransportPosition {
    static constexpr auto replay_mode = TickReplayMode::enabled;

    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{output<FractionalGlobalIndex>(
            {"position", "Position"}, OutputProduction::sequential)};
    }

    void tick(TickSampleContext<TransportPosition> const& ctx) const
    {
        ctx.output<"position">().push(ctx.index);
    }
};

namespace details {

template<ChannelTypeId Channels, class Source, class Write>
void resample_linear(Source const& source, std::optional<GlobalLatticeInterpolation> taps, Write write)
{
    if (!taps) {
        for (std::size_t channel = 0; channel < channel_count(Channels); ++channel)
            write(channel, Sample{});
        return;
    }
    auto window_at = [&](GlobalIndex index) {
        auto window = source.prepared_window_at(index);
        IV_ASSERT(window || !source.coverage().contains(index),
            "a covered resampler tap must be prepared before Tick");
        return window;
    };
    auto const left = window_at(taps->lower);
    auto const right = taps->fraction ? window_at(taps->upper) : std::nullopt;
    auto const weight = static_cast<double>(taps->fraction) * 0x1p-64;
    for (std::size_t channel = 0; channel < channel_count(Channels); ++channel) {
        auto const a = left ? left->at(taps->lower, channel).value : 0.f;
        auto const b = right ? right->at(taps->upper, channel).value : 0.f;
        write(channel, taps->fraction
            ? Sample{static_cast<float>(std::lerp(static_cast<double>(a), static_cast<double>(b), weight))}
            : Sample{a});
    }
}

template<ChannelTypeId Channels, SampleStreamLayout Layout, InputContentAccess Content>
constexpr auto linear_resampler_inputs()
{
    return std::tuple{
        input<FractionalGlobalIndex>({"position", "Position"}, InputAccess::sequential),
        input<Sample>({"source", "Source"}, InputAccess::coverage,
            Content, {.channel_layout = {Channels, Layout}}),
    };
}

} // namespace details

template<ChannelTypeId Channels, SampleStreamLayout Layout>
struct LinearResamplerNode {
    static constexpr auto replay_mode = TickReplayMode::enabled;

    static constexpr ChannelLayout layout{Channels, Layout};

    static constexpr auto inputs()
    {
        return details::linear_resampler_inputs<Channels, Layout, InputContentAccess::complete_buffer>();
    }

    static constexpr auto outputs()
    {
        return std::array{output<Sample>({"out", "Out"}, OutputProduction::sequential,
            {.channel_layout = layout})};
    }

    void constrain_ports(ConstrainPortsContext<LinearResamplerNode>& ctx) const
    {
        ctx.template input<"position">().pace() = ctx.template output<"out">().pace();
    }

    void tick(TickSampleContext<LinearResamplerNode> const& ctx) const
    {
        auto const out = ctx.template output<"out">();
        auto const source = ctx.template input<"source">();
        auto const taps = ctx.template input_has_coverage<"position">()
            ? interpolate_global_lattice(ctx.template input<"position">().get(), source.absolute_pace_exponent())
            : std::nullopt;
        details::resample_linear<Channels>(source, taps,
            [&](std::size_t channel, Sample value) { out(channel) = value; });
    }
};

template<ChannelTypeId Channels, SampleStreamLayout Layout>
struct DeferredLinearResamplerNode {
    struct PendingTaps {
        GlobalLatticeInterpolation taps{};
        bool present = false;
    };

    struct State {
        std::span<PendingTaps> pending;
        std::size_t cursor = 0;
        std::size_t samples_submitted = 0;
        std::uint64_t generation = 0;
    };

    std::size_t preparation_samples;
    explicit DeferredLinearResamplerNode(std::size_t samples) : preparation_samples(samples) {
        if (samples == 0) throw std::invalid_argument("computed resampling needs preparation samples");
    }

    static constexpr auto inputs() {
        return details::linear_resampler_inputs<Channels, Layout, InputContentAccess::windowed>();
    }
    static constexpr auto outputs() { return LinearResamplerNode<Channels, Layout>::outputs(); }

    void declare(DeclarationContext<DeferredLinearResamplerNode> const& ctx) const {
        ctx.template local_array<&State::pending>(preparation_samples);
    }

    void constrain_ports(ConstrainPortsContext<DeferredLinearResamplerNode>& ctx) const {
        ctx.template input<"position">().pace() = ctx.template output<"out">().pace();
        ctx.template output<"out">().latency() = preparation_samples;
    }

    void tick(TickSampleContext<DeferredLinearResamplerNode> const& ctx) const
    {
        auto& state = ctx.state();
        auto const source = ctx.template input<"source">();
        if (state.generation != source.request_generation()) {
            state.generation = source.request_generation();
            state.cursor = 0;
            state.samples_submitted = 0;
        }
        auto const out = ctx.template output<"out">();
        auto& pending = state.pending[state.cursor];
        if (state.samples_submitted == preparation_samples) {
            details::resample_linear<Channels>(source, pending.present ? std::optional{pending.taps} : std::nullopt,
                [&](std::size_t channel, Sample value) { out.update(value, preparation_samples - 1, channel); });
        } else {
            ++state.samples_submitted;
        }
        pending.present = false;
        if (ctx.template input_has_coverage<"position">()) {
            auto const next = interpolate_global_lattice(ctx.template input<"position">().get(),
                source.absolute_pace_exponent());
            if (next) {
                std::array const positions{next->lower, next->upper};
                if (ctx.template request_input<"source">({.individual_positions =
                        std::span{positions}.first(next->fraction ? 2 : 1)})) {
                    pending = {.taps = *next, .present = true};
                }
            }
        }
        for (std::size_t channel = 0; channel < channel_count(Channels); ++channel)
            out(channel) = Sample{};
        if (++state.cursor == preparation_samples) state.cursor = 0;
    }
};

using LinearResampler = LinearResamplerNode<ChannelTypeId::mono, SampleStreamLayout::planar>;
using StereoLinearResampler = LinearResamplerNode<ChannelTypeId::stereo, SampleStreamLayout::interleaved>;

} // namespace iv
