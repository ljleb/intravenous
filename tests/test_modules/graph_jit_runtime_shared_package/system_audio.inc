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
    std::size_t history = 0;

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

    void constrain_ports(iv::ConstrainPortsContext<EarlyTickAudioRamp>& ctx) const
    {
        ctx.output<"out">().history() = history;
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

void retained_early_tick_audio(iv::GraphBuilder& graph)
{
    auto ramp = graph.node<EarlyTickAudioRamp>(std::size_t{129});
    auto output = graph.node<iv::SystemAudioOutput>();
    output(ramp);
    graph.outputs();
}

IV_MODULE("iv.test.graph_jit.retained_early_tick_audio", retained_early_tick_audio, "Retained Early Tick Audio");

struct RevisableCutoverSample {
    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>({"in", "In"}, iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>({"out", "Out"}, iv::OutputProduction::sequential)};
    }

    void constrain_ports(iv::ConstrainPortsContext<RevisableCutoverSample>& ctx) const
    {
        // Keep the output in a canonical ring across the two reserved ranges.
        ctx.output<"out">().history() = 64;
        ctx.output<"out">().latency() = 1;
    }

    void tick(iv::TickSampleContext<RevisableCutoverSample> const& ctx) const
    {
        auto const out = ctx.output<"out">();
        out.update(ctx.input<"in">().get());
        out() = iv::Sample{};
    }
};

struct LiveInvertingSampleDelay {
    static constexpr bool is_live = true;
    struct State { float pending = 0.f; };

    static constexpr auto inputs() { return RevisableCutoverSample::inputs(); }
    static constexpr auto outputs() { return RevisableCutoverSample::outputs(); }

    void tick(iv::TickSampleContext<LiveInvertingSampleDelay> const& ctx) const
    {
        auto& pending = ctx.state().pending;
        ctx.output<"out">()() = iv::Sample{pending};
        pending = ctx.input<"in">().get().value == 0.f ? 1.f : 0.f;
    }
};

struct CutoverSampleProbe {
    struct State { float first = 0.f; float last = 0.f; };

    static constexpr auto inputs() { return RevisableCutoverSample::inputs(); }
    static constexpr auto outputs() { return std::tuple{}; }

    void tick_block(iv::TickBlockContext<CutoverSampleProbe> const& ctx) const
    {
        auto const values = ctx.input<"in">().block();
        ctx.state() = {values[0].value, values[values.size() - 1].value};
    }
};

void reversed_tick_dependency(iv::GraphBuilder& graph, bool reverse = false)
{
    auto early = graph.node<RevisableCutoverSample>();
    auto live = graph.node<LiveInvertingSampleDelay>();
    auto probe = graph.node<CutoverSampleProbe>();
    auto device = graph.node<iv::SystemAudioOutput>();
    if (reverse) early(live);
    else live(early);
    probe(early);
    device(early);
    graph.outputs();
}

IV_MODULE("iv.test.graph_jit.reversed_tick_dependency", reversed_tick_dependency, "Reversed Tick Dependency");
