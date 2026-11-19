// The application ships this as one ordinary IV package.  Keeping the
// built-ins behind the same registration/ORC boundary as user packages makes
// their implementation replaceable and prevents source modules from reaching
// into iv_builder with g.node<NodeType>().
#include <intravenous/dsl.h>

#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/basic_nodes/automation.h>
#include <intravenous/basic_nodes/beats.h>
#include <intravenous/basic_nodes/timeline.h>
#include <intravenous/basic_nodes/constant.h>
#include <intravenous/basic_nodes/debug_probe.h>
#include <intravenous/basic_nodes/filters.h>
#include <intravenous/basic_nodes/noise.h>
#include <intravenous/basic_nodes/output_gain.h>
#include <intravenous/basic_nodes/polyphonic.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/basic_nodes/shaping.h>
#include <intravenous/basic_nodes/system_audio.h>
#include <intravenous/basic_nodes/timing.h>

IV_NODE("iv.builtin.constant", iv::Constant, "Constant");
IV_REGISTER_OUTPUT_GAIN_NODES();
IV_NODE("iv.builtin.debug_probe", ::DebugProbe, "Debug Probe");

IV_NODE("iv.builtin.automation.point_source", iv::automation::PointSource, "Automation Points");
IV_NODE("iv.builtin.automation.interpreter", iv::automation::Curve, "Automation Interpreter");
IV_MODULE_CLASS("iv.builtin.automation.curve", iv::automation::AutomationCurve, "Automation Curve");

IV_MODULE_CLASS("iv.builtin.timeline", iv::Timeline, "Timeline");
IV_NODE("iv.builtin.beats", iv::Beats, "Beats");

IV_NODE("iv.builtin.invert", iv::Invert, "Invert");
IV_NODE("iv.builtin.power", iv::Power, "Power");
IV_NODE("iv.builtin.subtract", iv::Subtract, "Subtract");
IV_NODE("iv.builtin.quotient", iv::Quotient, "Quotient");

IV_NODE("iv.builtin.whack_iir", iv::WhackIirThing, "Whack IIR");
IV_NODE("iv.builtin.simple_iir_high_pass", iv::SimpleIirHighPass, "Simple IIR High Pass");
IV_NODE("iv.builtin.simple_iir_low_pass", iv::SimpleIirLowPass, "Simple IIR Low Pass");

IV_NODE("iv.builtin.uniform_noise", iv::UniformNoise, "Uniform Noise");
IV_NODE("iv.builtin.deterministic_uniform_noise", iv::DeterministicUniformNoise, "Deterministic Uniform Noise");
IV_NODE("iv.builtin.deterministic_uniform_aes_noise", iv::DeterministicUniformAESNoise, "Deterministic Uniform AES Noise");
IV_NODE("iv.builtin.uniform_to_cauchy", iv::UniformToCauchy, "Uniform To Cauchy");
IV_NODE("iv.builtin.uniform_to_power", iv::UniformToPower, "Uniform To Power");
IV_NODE("iv.builtin.uniform_to_gaussian", iv::UniformToGaussian, "Uniform To Gaussian");
IV_NODE("iv.builtin.deterministic_gaussian_aes_noise", iv::DeterministicGaussianAESNoise, "Deterministic Gaussian AES Noise");

IV_NODE("iv.builtin.dummy_sink", iv::DummySink, "Dummy Sink");
IV_NODE("iv.builtin.dummy_event_sink", iv::DummyEventSink, "Dummy Event Sink");

IV_NODE("iv.builtin.warper", iv::Warper, "Warper");
IV_NODE("iv.builtin.phase_integrator", iv::PhaseIntegrator, "Phase Integrator");
IV_NODE("iv.builtin.saw_oscillator", iv::SawOscillator, "Saw Oscillator");
IV_NODE("iv.builtin.sine_oscillator", iv::SineOscillator, "Sine Oscillator");
IV_NODE("iv.builtin.phase_offset_predictor", iv::PhaseOffsetPredictor, "Phase Offset Predictor");
IV_NODE("iv.builtin.interpolation", iv::Interpolation, "Interpolation");

IV_NODE("iv.builtin.latency", iv::Latency, "Latency");
IV_NODE("iv.builtin.midi_pitch", iv::MidiPitch, "MIDI Pitch");
IV_NODE("iv.builtin.midi_gate", iv::MidiGate, "MIDI Gate");

IV_NODE("iv.builtin.system_audio_input", iv::SystemAudioInput, "System Audio Input");
IV_NODE("iv.builtin.system_audio_output", iv::SystemAudioOutput, "System Audio Output");

// Generic node families and compiler-inserted dynamic-arity helpers deliberately
// have no registered ID. Their concrete specializations are internal
// lowering/compiler implementation details; a future source-facing
// specialization gets its own explicit ID.
