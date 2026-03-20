#include "pch.h"
#include "public.h"
#include "dsl.h"
#include "midi_node.h"
#include "graph_node.h"
#include <any>


template<class T>
class Knob {
    std::atomic<T>* _value;

public:
    explicit Knob(std::atomic<T>* value) :
        _value(value)
    {}

    constexpr auto outputs() const
    {
        return std::array<iv::OutputConfig, 1>{};
    }

    void tick(iv::TickState const& state) {
        auto& out = state.outputs[0];
        out.push(_value->load(std::memory_order::relaxed));
    }
};

class AudioStream {
    iv::Sample*& _destination;

public:
    constexpr explicit AudioStream(iv::Sample*& destination) :
        _destination(destination)
    {}

    constexpr auto inputs() const
    {
        return std::array<iv::InputConfig, 1>{};
    }

    void tick(iv::TickState const& state)
    {
        auto& in = state.inputs[0];
        *_destination = std::fmin(std::fmax(in.get(), -1.0), 1.0);
    }
};

struct WhackIirThing {
    double const* _update_frequency;

public:
    constexpr explicit WhackIirThing(double const* update_frequency) :
        _update_frequency(update_frequency)
    {}

    constexpr auto inputs() const
    {
        return std::array<iv::InputConfig, 3>{};
    }

    constexpr auto outputs() const
    {
        return std::array<iv::OutputConfig, 2>{};
    }

    void tick(iv::TickState const& state) const
    {
        iv::Sample in_dry = state.inputs[0].get();
        iv::Sample in_control = state.inputs[1].get();
        iv::Sample dx = *_update_frequency;
        iv::Sample alpha = 1 - in_control; for (size_t i = 0; i < 4; ++i) alpha *= alpha;

        auto& out_low = state.outputs[0];
        auto& out_high = state.outputs[1];
        iv::Sample last_low = out_low.get();

        iv::Sample low = last_low + alpha * (in_dry - last_low);
        out_low.push(low);
        out_high.push(in_dry - low);
    }
};

struct SimpleIirHighPass {
    double const* _sample_period;

public:
    static constexpr iv::Sample const FMIN = 1;
    static constexpr iv::Sample const FMAX = 4.41e4;

    constexpr explicit SimpleIirHighPass(double const* sample_period) :
        _sample_period(sample_period)
    {}

    constexpr auto inputs() const
    {
        return std::array {
            iv::InputConfig { .history = 1 },
            iv::InputConfig{},
        };
    }

    constexpr auto outputs() const
    {
        return std::array<iv::OutputConfig, 1>{};
    }

    void tick(iv::TickState const& state) const
    {
        auto& in = state.inputs[0];
        auto& ctrl = state.inputs[1];
        auto& out = state.outputs[0];

        // read your 0…1 knob
        iv::Sample u = ctrl.get();                   // ∈ [0,1]

        // compute host rate and the *usable* max cutoff
        iv::Sample dx = *_sample_period;
        iv::Sample usableMax = std::min<iv::Sample>(FMAX, 0.5 / dx);

        // 2) (optionally) exponential sweep, clamped
        iv::Sample f_c = FMIN * std::pow(usableMax / FMIN, u);

        // normalize and compute IIR coeffs
        iv::Sample norm = f_c * dx;                     // ∈ [0, usableMax/fs] ⩽ 0.5
        iv::Sample c = std::tan(std::_Pi_val * norm);
        iv::Sample a1 = (1.0 - c) / (1.0 + c);
        iv::Sample b0 = 1.0 / (1.0 + c);

        // standard one‑pole highpass:
        iv::Sample x = in.get(0);
        iv::Sample x1 = in.get(1);
        iv::Sample y1 = out.get();
        iv::Sample y = a1*y1 + b0*(x - x1);

        out.push(y);
    }
};

struct SimpleIirLowPass {
    double const* _sample_period;

public:
    static constexpr iv::Sample const FMIN = 2e1;
    static constexpr iv::Sample const FMAX = 4.41e4;

    constexpr explicit SimpleIirLowPass(double const* sample_period) :
        _sample_period(sample_period)
    {}

    constexpr auto inputs() const
    {
        return std::array {
            iv::InputConfig { .name = "in", .history=1 },
            iv::InputConfig { .name = "cutoff" },
        };
    }

    constexpr auto outputs() const
    {
        return std::array {
            iv::OutputConfig { "out" },
        };
    }

    void tick(iv::TickState const& state) const
    {
        auto& in = state.inputs[0];
        auto& ctrl = state.inputs[1];
        auto& out = state.outputs[0];

        // 1) read your 0…1 knob
        iv::Sample u = ctrl.get();          // ∈ [0,1]

        // 2) compute sample‐rate and clamped max cutoff
        iv::Sample dx = *_sample_period;
        iv::Sample usableMax = std::min<iv::Sample>(FMAX, 0.5 / dx);

        // 3a) linear sweep: maps 0→1 straight to FMIN→usableMax
        //iv::Sample fc_linear = FMIN + u * (usableMax - FMIN);

        // 3b) exponential sweep: perceptually smoother
        iv::Sample f_c = FMIN * std::pow(usableMax / FMIN, u);

        // 4) normalize to [0…0.5] and compute warped c
        iv::Sample norm = f_c * dx;             // now ∈ [FMIN/fs … usableMax/fs] ⩽ 0.5
        iv::Sample c = std::tan(std::_Pi_val * norm);

        // 5) one‑pole low‑pass coeffs
        iv::Sample a1 = (1.0 - c) / (1.0 + c);
        iv::Sample alpha = c / (1.0 + c);

        // 6) apply difference equation
        iv::Sample x = in.get(0);
        iv::Sample x_prev = in.get(1);
        iv::Sample y_prev = out.get();

        iv::Sample y = a1*y_prev + alpha*(x + x_prev);

        out.push(y);
    }
};

static iv::Graph feedback_voice(
    double* sample_period
) {
    using namespace iv;
    GraphBuilder g;

    auto amplitude = g.input("amplitude");
    auto frequency = g.input("frequency");
    auto reset = g.input("reset");

    auto voice_noise = g.input("voice_noise");

    auto integrator = g.node<Integrator>(sample_period);
    auto warper = g.node<Warper>();

    integrator(warper["aliased"].detach(), frequency * 2.0, reset);
    warper(integrator + voice_noise);
    g.outputs(amplitude * warper["anti_aliased"]);
    auto res = std::move(g).build();
    return res;
}

auto iv::init_graph(
    double* sample_period,
    Sample* channels[2],
    std::atomic<float>* noise_level,
    std::atomic<float>* gaussian_noise_ratio,
    std::atomic<float>* warp_threshold,
    std::atomic<float>* noise_lo_pass,
    std::atomic<float>* noise_hi_pass) -> NodeProcessor*
{
    GraphBuilder g;
    std::array<NodeRef, 2> noises;
    for (size_t i = 0; i < noises.size(); ++i)
    {
        noises[i] = g.subgraph([&](auto& g)
        {
            auto level_knob = g.node<Knob<float>>(noise_level);
            auto lo_pass_knob = g.node<Knob<float>>(noise_lo_pass);
            auto hi_pass_knob = g.node<Knob<float>>(noise_hi_pass);
            auto generator = g.node<DeterministicUniformAESNoise>(-1, 1, 0ull);
            auto lo_pass = g.node<SimpleIirLowPass>(sample_period);
            auto hi_pass = g.node<SimpleIirHighPass>(sample_period);

            auto u_to_n_knob = g.node<Knob<float>>(gaussian_noise_ratio);
            auto u_to_n = g.node<UniformToGaussian>(0.0, 0.5);
            auto u_to_c = g.node<UniformToCauchy>(0.0, 0.01);
            auto interp = g.node<Interpolation>();

            u_to_c(generator);
            u_to_n(generator);

            interp(u_to_n, u_to_c, u_to_n_knob);

            lo_pass(interp, lo_pass_knob);
            hi_pass(lo_pass, hi_pass_knob);

            g.outputs(hi_pass * level_knob);
        });
    }

    auto midi_voice = feedback_voice(sample_period);

    auto midi_left = g.node<MidiNode>(midi_voice);
    auto midi_right = g.node<MidiNode>(midi_voice);

    auto left_out = g.node<AudioStream>(channels[0]);
    auto right_out = g.node<AudioStream>(channels[1]);

    midi_left(noises[0]);
    midi_right(noises[1]);
    left_out(midi_left);
    right_out(midi_right);

    g.outputs();

    return new NodeProcessor(std::move(g).build());
}

void iv::tick(NodeProcessor* processor, std::span<MidiMessage const> midi, size_t index)
{
    processor->tick(midi, index);
}

void iv::free_graph(NodeProcessor* processor)
{
    delete processor;
}
