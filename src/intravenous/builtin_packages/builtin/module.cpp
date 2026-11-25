// The application ships this as one ordinary IV package. Its exported
// definitions and private leaves use the same compiler records as user packages.
#include <intravenous/dsl.h>

#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/basic_nodes/automation.h>
#include <intravenous/basic_nodes/beats.h>
#include <intravenous/basic_nodes/click_detector.h>
#include <intravenous/basic_nodes/timeline.h>
#include <intravenous/basic_nodes/constant.h>
#include <intravenous/basic_nodes/debug_probe.h>
#include <intravenous/basic_nodes/filters.h>
#include <intravenous/basic_nodes/noise.h>
#include <intravenous/basic_nodes/output_gain.h>
#include <intravenous/basic_nodes/polyphonic.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/basic_nodes/sampler.h>
#include <intravenous/basic_nodes/shaping.h>
#include <intravenous/basic_nodes/system_audio.h>
#include <intravenous/basic_nodes/timing.h>

IV_NODE("builtin.constant", iv::Constant, "Constant");
IV_INSTANTIATE_OUTPUT_GAIN_NODES();
IV_NODE("builtin.debug_probe", ::DebugProbe, "Debug Probe");
IV_NODE("builtin.click_detector", iv::ClickDetector, "Click Detector");

IV_MODULE_CLASS("builtin.automation.curve", iv::automation::AutomationCurve, "Automation Curve", iv::tags(iv::tag::automation));

IV_MODULE_CLASS("builtin.timeline", iv::Timeline, "Timeline", iv::tags(iv::tag::timeline));
IV_NODE("builtin.beats", iv::Beats, "Beats", iv::tags(iv::tag::rhythm, iv::tag::beats));

IV_NODE("builtin.invert", iv::Invert, "-a");
IV_NODE("builtin.power", iv::Power, "a ^ b");
IV_NODE("builtin.sum", iv::Sum, "a + b");
IV_NODE("builtin.subtract", iv::Subtraction, "a - b");
IV_NODE("builtin.product", iv::Product, "a * b");
IV_NODE("builtin.quotient", iv::Quotient, "a / b");

IV_NODE("builtin.whack_iir", iv::WhackIirThing, "Whack IIR");
IV_NODE("builtin.simple_iir_high_pass", iv::SimpleIirHighPass, "Simple IIR High Pass");
IV_NODE("builtin.simple_iir_low_pass", iv::SimpleIirLowPass, "Simple IIR Low Pass");

IV_NODE("builtin.uniform_noise", iv::UniformNoise, "Uniform Noise");
IV_NODE("builtin.deterministic_uniform_noise", iv::DeterministicUniformNoise, "Deterministic Uniform Noise");
IV_NODE("builtin.deterministic_uniform_aes_noise", iv::DeterministicUniformAESNoise, "Deterministic Uniform AES Noise");
IV_NODE("builtin.uniform_to_cauchy", iv::UniformToCauchy, "Uniform To Cauchy");
IV_NODE("builtin.uniform_to_power", iv::UniformToPower, "Uniform To Power");
IV_NODE("builtin.uniform_to_gaussian", iv::UniformToGaussian, "Uniform To Gaussian");
IV_NODE("builtin.deterministic_gaussian_aes_noise", iv::DeterministicGaussianAESNoise, "Deterministic Gaussian AES Noise");

IV_NODE("builtin.dummy_sink", iv::DummySink, "Dummy Sink");
IV_NODE("builtin.dummy_event_sink", iv::DummyEventSink, "Dummy Event Sink");

IV_NODE("builtin.clip", iv::Clip, "Clip");
IV_NODE("builtin.stereo_clip", iv::StereoClip, "Stereo Clip");
IV_NODE("builtin.warper", iv::Warper, "Warper");
IV_NODE("builtin.phase_integrator", iv::PhaseIntegrator, "Phase Integrator");
IV_NODE("builtin.saw", iv::SawOscillator, "Saw Oscillator", iv::tags(iv::tag::oscillator));
IV_NODE("builtin.sine", iv::SineOscillator, "Sine Oscillator", iv::tags(iv::tag::oscillator));
IV_NODE("builtin.phase_offset_predictor", iv::PhaseOffsetPredictor, "Phase Offset Predictor");
IV_NODE("builtin.interpolation", iv::Interpolation, "Interpolation");

IV_NODE("builtin.latency", iv::Latency, "Latency");
IV_NODE("builtin.midi_pitch", iv::MidiPitch, "MIDI Pitch");
IV_NODE("builtin.midi_gate", iv::MidiGate, "MIDI Gate");

IV_NODE("builtin.system_audio_input", iv::SystemAudioInput, "System Audio Input", iv::tags(iv::tag::device, iv::tag::input));
IV_NODE("builtin.system_audio_output", iv::SystemAudioOutput, "System Audio Output", iv::tags(iv::tag::device, iv::tag::output));
IV_NODE("builtin.sampler", iv::Sampler, "Sampler");

// Generic node families and compiler-inserted dynamic-arity helpers deliberately
// have no registered ID. Their concrete specializations are internal
// lowering/compiler implementation details; a future source-facing
// specialization gets its own explicit ID.
