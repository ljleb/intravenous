#include "basic_nodes.h"
#include "node.h"
#include "dsl.h"
#include "graph_node.h"
#include "wav.h"


using namespace iv;


static void feedback_voice(GraphBuilder& g) {
    auto amplitude = g.input("amplitude", 0.5);
    auto frequency = g.input("frequency", 1000.0);
    auto voice_noise = g.input("noise");
    auto dx = g.input("dx", 1.0);
    auto reset = g.input("reset");

    auto integrator = g.node<Integrator>();
    auto warper = g.node<Warper>();

    integrator(warper["aliased"].detach(), frequency * 2.0, dx, reset);
    warper(integrator + voice_noise);
    g.outputs(warper["anti_aliased"] * amplitude);
}

static void noise_voice(GraphBuilder& g) {
    auto const dx = g.input("dx", 1.0);

    auto const level_knob = 0.5;
    auto const lo_pass_knob = 1.0;
    auto const hi_pass_knob = 0.9;
    auto const generator = g.node<DeterministicUniformAESNoise>(0ull);
    auto const lo_pass = g.node<SimpleIirLowPass>();
    auto const hi_pass = g.node<SimpleIirHighPass>();

    auto const u_to_n_knob = 0.0;
    auto const u_to_n = g.node<UniformToGaussian>(0.0, 0.5);
    auto const u_to_c = g.node<UniformToCauchy>(0.0, 0.01);
    auto const interp = g.node<Interpolation>();

    generator({ {"min", -1}, {"max", 1} });

    u_to_c(generator);
    u_to_n(generator);

    interp(u_to_n, u_to_c, u_to_n_knob);

    lo_pass(interp, lo_pass_knob, dx);
    hi_pass(lo_pass, hi_pass_knob, dx);

    g.outputs(hi_pass * level_knob);
}

NodeProcessor init_graph(
    Sample* sample_period_buffer,
    Sample** channels,
    size_t channels_size,
    size_t num_channels
) {
    GraphBuilder g;

    auto const sample_period = g.node<BufferSource>(sample_period_buffer, 1);

    for (size_t i = 0; i < num_channels; ++i)
    {
        auto const noise = g.subgraph(noise_voice);
        auto const voice = g.subgraph(feedback_voice);
        auto const out = g.node<BufferSink>(channels[i], channels_size);

        noise({{"dx", sample_period}});
        voice(0.5f, 200.0f, noise, sample_period);
        out(voice);
    }

    g.outputs();

    return NodeProcessor(std::move(g).build());
}


int main() {
    size_t const sample_rate = 48000;
    float const duration_seconds = 4.0f;

    size_t const num_samples = static_cast<size_t>(sample_rate * duration_seconds);
    Sample sample_period = 1.0f / static_cast<Sample>(sample_rate);

    std::vector<Sample> left(num_samples);
    std::vector<Sample> right(num_samples);

    Sample* channels[2] = { left.data(), right.data() };
    auto processor = init_graph(&sample_period, channels, num_samples, 2);

    for (size_t i = 0; i < num_samples; ++i) {
        processor.tick({}, i);
    }

    write_wav("out.wav", left, right, sample_rate);
}
