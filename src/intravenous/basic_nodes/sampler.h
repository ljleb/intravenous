#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>
#include <string_view>
#include <tuple>

namespace iv {

struct Sampler {
    char const* directory;

    constexpr explicit Sampler(char const* directory_ = "recordings")
        : directory(directory_)
    {}

    static constexpr ChannelLayout layout{
        .channel_type = ChannelTypeId::stereo,
        .sample_layout = SampleStreamLayout::interleaved,
    };

    static constexpr auto inputs()
    {
        return std::array{
            input<Sample>("audio", InputAccess::sequential, {.channel_layout = layout}),
            input<Sample>("record", InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{output<Sample>("recorded", OutputProduction::sequential,
            OutputRealization::buffered, {.channel_layout = layout})};
    }

    void constrain_ports(ConstrainPortsContext<Sampler>& ctx) const
    {
        ctx.input<"audio">().pace().absolute() = 1;
        ctx.input<"record">().pace().absolute() = 1;
        ctx.output<"recorded">().pace().absolute() = 1;
    }

    void declare(DeclarationContext<Sampler>& ctx) const
    {
        ctx.record_output_to<"recorded">(directory ? std::string_view{directory} : std::string_view{});
    }

    void tick_block(TickBlockContext<Sampler> const& ctx) const
    {
        auto const armed = ctx.input<"record">().get_value(0).value >= 0.5f;
        if (!armed) return;

        auto recorded = ctx.output<"recorded">();
        auto const audio = ctx.input<"audio">();
        for (std::size_t frame = 0; frame < recorded.block_size(); ++frame) {
            for (std::size_t channel = 0; channel < channel_count(layout.channel_type); ++channel)
                recorded.value(frame, channel) = audio.get_value(frame, channel);
        }
    }

    void skip_block(SkipBlockContext<Sampler> const& ctx) const
    {
        tick_block(ctx);
    }
};

} // namespace iv
