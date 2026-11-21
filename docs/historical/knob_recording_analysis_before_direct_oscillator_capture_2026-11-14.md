# Knob Recording Analysis Before The Direct Oscillator Capture

Status: historical observations, including an incorrect conclusion.

Preserved on 2026-11-14 from the final section of
[Vue Client Implementation Stages](../vue_client_implementation_stages.md).
The first recording included the HRTF panner. The direct oscillator recording
subsequently exposed a phase-unit error in `SineOscillator::tick()`. In particular,
the claim that continuous re-anchoring ruled out an oscillator defect was wrong.
The current investigation and replacement status are in the source document.
Block handoff times measure callback spacing, not individual render durations.

Knob recording analysis (2026-11-14; observation, not a requirement):

- The owner's recording `/tmp/iv-knob-audio-f2a2c261-c6c4-42a3-803f-fdd2740add19`
  spans 11.9 s of wall time for 8 s of audio: 28 render blocks took about 150 ms
  instead of 5.3 ms. They form 12 episodes. Each begins 22-123 ms after an
  `elevation` or `azimuth` gesture begins and lasts until about one control compile
  after it ends. The device underruns throughout, which is audible as clicks. The one
  `freq` gesture caused no stall.
- Gesture updates now leave the client every 16.8 ms (median), without bursts.
- Cause, from source: `LearnedHrtfSourceIcosphere::tick_block()` in
  `projects/simple_sine/modules/q24_icosphere_pan/module.cpp` calls
  `update_direction()` whenever a sample's direction differs from the previous
  sample's. While dynamic code is active, `stage_manual_control_sample_timelines()`
  ramps the manual value across every sample of the block, so both stereo
  instances recompute the HRTF direction about 512 times per 256-sample block. A
  settled constant never triggers it. Earlier bursty delivery left many blocks
  unramped, which hid the cost.
- The per-sample ramp is the normative contract. The cost belongs to the node's
  choice of per-sample direction updates. A control-rate direction update in the
  panner is the proposed remedy; the owner's decision is pending.
- The stalls are not the only click source. The owner hears clicks with every knob.
  `freq` clicks only while the value moves; `azimuth` also clicks while held still.
  In the WAV, each of the three applied `freq` updates is followed, in the next
  block, by a burst on the right output only. It starts about five samples in, with
  a second difference about five times the baseline. The left output stays clean.
  `SineOscillator::tick()` re-anchors its phase continuously when the frequency
  changes, so the burst arises downstream of the oscillators. The mechanism is not
  yet identified. The recording has no long still hold of `azimuth`.
