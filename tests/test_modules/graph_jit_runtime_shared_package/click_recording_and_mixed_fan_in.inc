#include <intravenous/basic_nodes/click_detector.h>
#include <intravenous/basic_nodes/constant.h>
#include <cmath>
#include <numbers>

namespace {
struct ClickRecordingSource {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        using namespace iv;
        return std::array{output<Sample>({"audio", "Audio"}, OutputProduction::sequential,
            {.channel_layout = system_audio_channel_layout})};
    }

    void tick_block(iv::TickBlockContext<ClickRecordingSource> const& ctx) const
    {
        auto const samples_per_half_second = static_cast<std::uint64_t>(ctx.sample_rate) / 2;
        auto const first = ctx.index.whole().bits();
        auto audio = ctx.output<"audio">();
        for (std::size_t i = 0; i < audio.block_size(); ++i) {
            auto const frame = first + i;
            auto const tone = 0.1 * std::sin(2 * std::numbers::pi * 220 * frame / ctx.sample_rate);
            auto const click = frame % samples_per_half_second == 13;
            audio.value(i, 0) = iv::Sample{tone};
            audio.value(i, 1) = iv::Sample{tone + (click ? 0.25 : 0.0)};
        }
    }
};
}

IV_NODE("iv.test.click_recording.source", ClickRecordingSource, "Click recording source");
IV_NODE("builtin.click_detector", iv::ClickDetector, "Click Detector");

void mixed_sample_fan_in_module(iv::GraphBuilder& graph)
{
    auto realtime = graph.node<"iv.test.graph_jit.state_context.native_batch_replay">(std::uint64_t{11});
    auto sparse = graph.node<"iv.test.graph_jit.state_context.single_region_sample_source">();
    auto probe = graph.node<"iv.test.replay.live_probe">();
    probe(realtime);
    probe(sparse);
    graph.outputs();
}
IV_MODULE("iv.test.replay.mixed_sample_fan_in", mixed_sample_fan_in_module, "Mixed sample fan-in");
