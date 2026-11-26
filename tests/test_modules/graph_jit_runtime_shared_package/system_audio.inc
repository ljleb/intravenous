#include <intravenous/basic_nodes/system_audio.h>

void system_audio_loopback(iv::GraphBuilder& graph)
{
    auto input = graph.node<iv::SystemAudioInput>();
    auto output = graph.node<iv::SystemAudioOutput>();
    output(input);
    graph.outputs();
}

IV_MODULE("iv.test.graph_jit.system_audio_loopback",
    system_audio_loopback, "System Audio Loopback");

struct EarlyTickAudioRamp {
    struct State {
        std::uint64_t produced = 0;
        iv::SystemAudioInputBinding test_gate;
    };

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>({"out", "Out"},
            iv::OutputProduction::sequential, {.channel_layout = iv::system_audio_channel_layout})};
    }

    void initialize(iv::InitializationContext<EarlyTickAudioRamp> const& ctx) const
    {
        ctx.state().test_gate = ctx.resources.system_audio.resolve_input("early_tick_test_gate");
    }

    void tick_block(iv::TickBlockContext<EarlyTickAudioRamp> const& ctx) const
    {
        auto output = ctx.output<"out">();
        auto& state = ctx.state();
        (void)state.test_gate.captured_samples();
        for (std::size_t frame = 0; frame < output.block_size(); ++frame) {
            auto const position = ctx.index.advanced_by({.whole = frame}).whole().signed_value();
            auto const value = iv::Sample{static_cast<float>(position) + static_cast<float>(++state.produced)};
            for (std::size_t channel = 0; channel < 2; ++channel)
                output.value(frame, channel) = value;
        }
    }
};

void early_tick_audio_mix(iv::GraphBuilder& graph)
{
    auto microphone = graph.node<iv::SystemAudioInput>();
    auto ramp = graph.node<EarlyTickAudioRamp>();
    auto output = graph.node<iv::SystemAudioOutput>();
    output(microphone + ramp);
    graph.outputs();
}

IV_MODULE("iv.test.graph_jit.early_tick_audio_mix", early_tick_audio_mix, "Early Tick Audio Mix");
