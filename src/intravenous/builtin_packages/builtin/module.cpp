// The application ships this as one ordinary IV package.  Keeping the
// built-ins behind the same registration/ORC boundary as user packages makes
// their implementation replaceable and prevents source modules from reaching
// into iv_builder with g.node<NodeType>().
#include <intravenous/dsl.h>

#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/basic_nodes/constant.h>
#include <intravenous/basic_nodes/debug_probe.h>
#include <intravenous/basic_nodes/filters.h>
#include <intravenous/basic_nodes/noise.h>
#include <intravenous/basic_nodes/polyphonic.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/basic_nodes/shaping.h>
#include <intravenous/basic_nodes/timing.h>

IV_NODE("iv.builtin.constant", iv::Constant);
IV_NODE("iv.builtin.debug_probe", ::DebugProbe);

IV_NODE("iv.builtin.invert", iv::Invert);
IV_NODE("iv.builtin.power", iv::Power);
IV_NODE("iv.builtin.subtract", iv::Subtract);
IV_NODE("iv.builtin.quotient", iv::Quotient);

IV_NODE("iv.builtin.whack_iir", iv::WhackIirThing);
IV_NODE("iv.builtin.simple_iir_high_pass", iv::SimpleIirHighPass);
IV_NODE("iv.builtin.simple_iir_low_pass", iv::SimpleIirLowPass);

IV_NODE("iv.builtin.uniform_noise", iv::UniformNoise);
IV_NODE("iv.builtin.deterministic_uniform_noise", iv::DeterministicUniformNoise);
IV_NODE("iv.builtin.deterministic_uniform_aes_noise", iv::DeterministicUniformAESNoise);
IV_NODE("iv.builtin.uniform_to_cauchy", iv::UniformToCauchy);
IV_NODE("iv.builtin.uniform_to_power", iv::UniformToPower);
IV_NODE("iv.builtin.uniform_to_gaussian", iv::UniformToGaussian);
IV_NODE("iv.builtin.deterministic_gaussian_aes_noise", iv::DeterministicGaussianAESNoise);

IV_NODE("iv.builtin.dummy_sink", iv::DummySink);
IV_NODE("iv.builtin.dummy_event_sink", iv::DummyEventSink);

IV_NODE("iv.builtin.warper", iv::Warper);
IV_NODE("iv.builtin.phase_integrator", iv::PhaseIntegrator);
IV_NODE("iv.builtin.saw_oscillator", iv::SawOscillator);
IV_NODE("iv.builtin.sine_oscillator", iv::SineOscillator);
IV_NODE("iv.builtin.phase_offset_predictor", iv::PhaseOffsetPredictor);
IV_NODE("iv.builtin.interpolation", iv::Interpolation);

IV_NODE("iv.builtin.latency", iv::Latency);
IV_NODE("iv.builtin.midi_pitch", iv::MidiPitch);
IV_NODE("iv.builtin.midi_gate", iv::MidiGate);

// Generic node families and compiler-inserted dynamic-arity helpers deliberately
// have no registered ID. Their concrete specializations are internal
// lowering/compiler implementation details; a future source-facing
// specialization gets its own explicit ID.
