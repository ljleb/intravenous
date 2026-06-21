# Audio Device Lanes Direction

This note captures the current intended first implementation direction for
audio devices as timeline lanes.

It is intentionally scoped to:

- one pacing stereo output device
- one non-pacing stereo input device
- timeline/task-runner integration
- adaptive synchronization of non-pacing audio into the pacing timeline

It does not yet cover:

- device enumeration and selection UI
- persistent project save/load for device choice
- more than one input or output device
- MIDI device lanes, though the synchronization seam here is intended to be
  reusable for them

## Core model

Exactly one realtime endpoint paces the task runner.

For the first audio implementation pass:

- one audio output device is the pacing endpoint
- the task runner blocks only at a pass boundary waiting for output demand
- every other realtime device is synchronized to that pacing timeline

The synchronization policy belongs to the owning app module, not to
`TasksRunner`.

## Task-runner pass boundary

`TasksRunner` should expose two pass-boundary events:

- `TasksRunnerBeforePassEvent`
- `TasksRunnerAfterPassEvent`

The naming makes their relative order explicit.

`TasksRunnerBeforePassEvent` runs:

- after the previous pass has fully completed
- before the next pass begins

Subscribers may block there.

For audio, the important consequence is:

- the pacing output device can block in its `BeforePass` handler waiting for a
  block request
- no task callback is ever blocked mid-pass waiting on a device

`TasksRunnerAfterPassEvent` runs after a pass completes and remains the natural
place for:

- structural reconciliation work
- output-buffer draining / response submission work

## AudioDeviceLanes app module

Introduce a dedicated app module:

- `AudioDeviceLanes`

For the first pass it owns:

- one `AudioOutputDevice`
- one `AudioInputDevice`
- one timeline output lane representing the master stereo output
- one timeline input lane representing captured stereo input
- pacing/output buffering state
- input capture buffering state
- input synchronization / adaptive resampling state

The module should stay isolated from the rest of the app through linker-event
bridges.

## Output lane

The output lane is a normal realtime sample lane inside `Timeline`.

Its lane node should:

- expose one realtime sample input
- expose one realtime sample output
- prefer interleaved stereo layout
- pass input through to output, filling silence when disconnected

`AudioDeviceLanes` does not need a special execution path for that lane.
`TimelineExecution` computes it like any other realtime sample lane.

After each pass, `AudioDeviceLanes` reads the output lane block from
`TimelineExecution`.

## Input lane

The input lane is also a normal realtime sample lane inside `Timeline`.

Its lane node should:

- expose no realtime inputs
- expose one realtime sample output
- prefer interleaved stereo layout
- fetch its current block from `AudioDeviceLanes`

That fetch should happen through a singleton event carrying a small builder,
similar to the existing graph-output lane path.

## Layout and channel type

At the device boundary we should use:

- `ChannelTypeId::stereo`
- `SampleStreamLayout::interleaved`

Reasons:

- audio devices typically prefer interleaved buffers
- miniaudio's resampler and data converter are interleaved
- timeline lane conversion machinery already handles consumer/producer layout
  adaptation on edges

The rest of the lane graph may still consume or produce planar blocks through
the existing conversion logic.

## Output pacing and block-size mismatch

The device block size and the timeline execution block size are not required to
match.

The pacing output path should therefore own a small interleaved stereo frame
reservoir.

### Before-pass behavior

On `TasksRunnerBeforePassEvent`:

1. If an output-device request is already pending and not yet fully satisfied,
   return immediately so another pass can run.
2. Otherwise wait for the next output-device block request.
3. Try to satisfy that request from already-rendered reservoir data.
4. If the request is fully satisfied, submit the response and continue waiting.
5. If it is not fully satisfied, keep it pending and allow the next pass to
   run.

This means the runner only executes when the pacing output path actually needs
more rendered frames.

### After-pass behavior

On `TasksRunnerAfterPassEvent`:

1. Read one timeline block from the output lane in interleaved stereo.
2. Append it to the output reservoir.
3. Copy as much as possible into the currently pending device request.
4. If the request becomes complete, submit the response to the device.
5. Advance the timeline realtime frontier by one timeline block.

This lets one device callback be satisfied by multiple timeline passes when the
device wants more frames than the timeline block size, while also allowing one
timeline pass to contribute partially to multiple future device requests when
the timeline block is larger.

## Input synchronization

The audio input device is not the pacing source.

Therefore its captured stream must be synchronized into the pacing timeline.

The important distinction is:

- pacing output device: defines when rendering is needed
- non-pacing input device: must adapt itself to the pacing timeline

That is where adaptive resampling belongs.

## Input capture thread

For the first pass, `AudioDeviceLanes` may own one dedicated input-capture
thread.

Reason:

- the current `AudioInputDevice` interface is blocking
- the input device should not pace the task runner
- a dedicated thread can wait for captured blocks and feed the synchronizer
  continuously

That thread should:

1. wait for `AudioInputDevice::wait_for_captured_block()`
2. push the captured interleaved stereo frames into the synchronizer
3. release the captured block

## Adaptive resampling state

Each non-pacing audio source should own a synchronization state object.

For the first pass that means the input side owns:

- a raw captured-frame FIFO
- a target buffered depth / latency
- a PI controller over buffer-depth error
- a miniaudio resampler

Its job is to output exactly one timeline block of interleaved stereo frames on
demand while slowly adjusting the input/output rate ratio so the raw FIFO stays
near the target fill.

### Control signal

The controller observes FIFO depth error:

- if the FIFO is too full, the resampler should consume input slightly faster
- if the FIFO is too empty, the resampler should consume input slightly slower

The resampler ratio should be:

- nominal ratio from declared device rate vs timeline rate
- multiplied by a small bounded correction from the PI controller

The correction must be clamped to a safe small range.

## Why this generalizes

This design intentionally separates:

- task-runner pacing
- device clock synchronization
- materialization of synchronized blocks or events

That same synchronization seam should later be reusable for MIDI:

- audio input uses adaptive sample resampling
- MIDI input uses timestamp-to-timeline-index projection

So the reusable concept is:

- non-pacing external devices synchronize themselves to the pacing timeline

not:

- every external device blocks or paces the task runner directly

## Testing direction

The synchronization/controller logic should be tested in two layers.

### Controller/synchronizer simulation tests

No real devices.

Feed synthetic scenarios such as:

- exact clock match
- source slightly faster
- source slightly slower
- bursty callback sizes
- callback jitter
- temporary capture dropout
- startup underfill / overfill

Check:

- convergence toward target FIFO fill
- bounded correction ratio
- limited overshoot
- no unstable oscillation
- acceptable recovery time

### End-to-end resampling tests

Use synthetic input audio, then verify:

- timeline blocks are produced continuously
- underruns/overruns stay controlled in tested scenarios
- long-run behavior tracks expected drift handling

These tests are important because the next consumer of the same general
cross-clock synchronization idea will be MIDI controller lanes.
