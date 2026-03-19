#include "pch.h"
#include "public.h"
#include "dsl.h"
#include "midi_node.h"
#include "graph_node.h"
#include <any>


template<class T>
class KnobNode {
    std::atomic<T>* _value;

public:
    explicit KnobNode(std::atomic<T>* value) :
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

class AudioStreamNode {
    iv::Sample*& _destination;

public:
    constexpr explicit AudioStreamNode(iv::Sample*& destination) :
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

static iv::GraphNode feedback_voice(
    double* sample_period
) {
    using namespace iv;
    GraphBuilder g;

    auto amplitude = g.input("amplitude");
    auto raw_frequency = g.input("frequency");
    auto reset = g.input("reset");
    auto voice_noise = g.input("voice_noise");

    auto integrator = g.node<Integrator>(sample_period);
    auto warper = g.node<WarperNode>();
    //auto predictor = g.node(iv::NlmsPredictor(1, 100, 1e-5, 1.0));
    //auto predictor = g.node(iv::TanhResidualPredictor(8, 8, 2, 4, 1e-6));
    //auto predictor = g.node(iv::TanhResidualAR2Predictor(0, 16, 16, 8, 8, 1e-8));
    //auto predictor = g.node(iv::PolyResidualPredictor(1, 16, 1e-6));
    //auto lo_pass = g.node(SimpleIirLowPass(sample_period));
    //auto lo_pass_coef = g.node(iv::ConstantNode(1.0));
    //auto latency = g.node(iv::Latency(100));
    //auto lo_pass2 = g.node(SimpleIirLowPass(sample_period));

    auto frequency = g.node<ProductNode>();
    auto constant = g.node<ConstantNode>(2.0);
    frequency(raw_frequency, constant);

    /*auto noise = make_subgraph_id(nodes, [](auto& nodes, auto& edges)
    {
        auto product = g.node(iv::ProductNode());
        auto product2 = g.node(iv::ProductNode());
        auto constant = g.node(iv::ConstantNode(1 / 440.0 / 2.0));

        edges.insert(iv::GraphEdge { { graph,    0 }, { product,  0 } });
        edges.insert(iv::GraphEdge { { graph,    1 }, { product,  1 } });
        edges.insert(iv::GraphEdge { { product,  0 }, { product2, 0 } });
        edges.insert(iv::GraphEdge { { constant, 0 }, { product2, 1 } });
        edges.insert(iv::GraphEdge { { product2, 0 }, { graph,    0 } });

        return std::make_tuple(2, 1);
    });*/

    //edges.insert(iv::GraphEdge { { frequency, 0 },                      { noise, 0 } });
    //edges.insert(iv::GraphEdge { { graph, voice_noise_generator_port }, { noise, 1 } });

    // low pass
    //edges.insert(iv::GraphEdge{ { lo_pass_coef, 0 }, { lo_pass, 1 } });
    //edges.insert(iv::GraphEdge{ { lo_pass_coef, 0 }, { lo_pass2, 1 } });

    // main loop
    //edges.insert(iv::GraphEdge { { integrator, 0 }, { lo_pass,    0 } });
    //edges.insert(iv::GraphEdge { { lo_pass,    0 }, { predictor,  0 } });
    //edges.insert(iv::GraphEdge { { predictor,    0 }, { lo_pass2,  0 } });
    //edges.insert(iv::GraphEdge { { lo_pass2,  0 }, { warper,     0 } });

    //edges.insert(iv::GraphEdge { { integrator, 0 }, { predictor,  0 } });
    //edges.insert(iv::GraphEdge { { integrator, 0 }, { lo_pass,    0 } });
    //edges.insert(iv::GraphEdge { { lo_pass,    0 }, { predictor,  0 } });

    //edges.insert(iv::GraphEdge { { predictor,  0 }, { warper,     0 } });
    auto noisy_integrator = g.node<SumNode>();
    noisy_integrator(integrator, voice_noise);
    warper(noisy_integrator);
    integrator(warper["anti_aliased"], frequency, reset);

    //edges.insert(iv::GraphEdge { { warper,     1 }, { lo_pass,    0 } });
    //edges.insert(iv::GraphEdge { { lo_pass,    0 }, { latency,    0 } });
    //edges.insert(iv::GraphEdge { { latency,    0 }, { predictor,  0 } });
    //edges.insert(iv::GraphEdge { { warper,  1 }, { latency, 0 } });
    //edges.insert(iv::GraphEdge { { warper,     1 }, { dummy_sink, 0 } });

    // knobs
    //edges.insert(iv::GraphEdge { { graph,     voice_warp_threshold_port },  { warper,     1 } });
    //edges.insert(iv::GraphEdge { { noise,     0 },                          { warper,     0 } });

    // out
    auto out_amplified = g.node<ProductNode>();
    g.outputs(out_amplified(amplitude, warper["aliased"]));

    return std::move(g).build();
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
    //auto warp_threshold_knob = g.node(KnobNode<float>(warp_threshold));
    //auto iir_outw0_knob = g.node(KnobNode<float>(_iir_outw0));

    std::array<NodeRef, 2> noises;
    for (size_t i = 0; i < noises.size(); ++i)
    {
        noises[i] = g.subgraph([&](auto& g)
        {
            auto level_knob = g.node<KnobNode<float>>(noise_level);
            auto lo_pass_knob = g.node<KnobNode<float>>(noise_lo_pass);
            auto hi_pass_knob = g.node<KnobNode<float>>(noise_hi_pass);
            auto generator = g.node<DeterministicUniformAESNoiseNode>(-1, 1, 0ull);
            auto product = g.node<ProductNode>();
            auto lo_pass = g.node<SimpleIirLowPass>(sample_period);
            auto hi_pass = g.node<SimpleIirHighPass>(sample_period);

            auto u_to_n_knob = g.node<KnobNode<float>>(gaussian_noise_ratio);
            auto u_to_n = g.node<UniformToGaussianNode>(0.0, 0.5);
            auto u_to_c = g.node<UniformToCauchyNode>(0.0, 0.01);
            auto interp = g.node<InterpolationNode>();

            u_to_c(generator);
            u_to_n(generator);

            interp(u_to_n, u_to_c, u_to_n_knob);

            lo_pass(interp, lo_pass_knob);
            hi_pass(lo_pass, hi_pass_knob);
            product(hi_pass, level_knob);

            g.outputs(product);
        });
    }

    auto midi_voice = feedback_voice(sample_period);

    auto midi_left = g.node<MidiNode>(midi_voice);
    auto midi_right = g.node<MidiNode>(midi_voice);

    auto left_out = g.node<AudioStreamNode>(channels[0]);
    auto right_out = g.node<AudioStreamNode>(channels[1]);

    // shared inputs
    //edges.insert(iv::GraphEdge { { warp_threshold_knob, 0 }, { midi_left, 0 } });
    //edges.insert(iv::GraphEdge { { warp_threshold_knob, 0 }, { midi_right, 0 } });

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
