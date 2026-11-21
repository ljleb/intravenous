# Audio continuity during observation changes

Investigation and implementation status recorded on 2026-11-21. Changing
observations must preserve DSP state and audio continuity when the
configured graph stays the same.

## Reproduction

The Saw project continued to produce audio with playback paused. A trace from
the VS Code graph editor recorded panning that changed the live meter probes.
The headless client replayed it against temporary copies of the project.
The replay used the original server bundle and existing package artifacts.
The server sent audio to PulseAudio on USB Audio Analog Stereo at 48 kHz.

The original capture had 29 graph switches and 29 corresponding clicks.
The fixed-viewport control had one graph switch. Neither run reported a
background underrun or a missed render budget. The playing replay also showed
matching Click Detector events, but the paused replay needed a direct sample
capture because its detector observations returned no events.

| Paused replay signal | Graph switches | Result at the switches |
| --- | ---: | --- |
| Saw routed directly to the device | 29 | 29 first stereo frames were zero |
| Concrete oscillator 2, with output gain | 29 | No click |
| Sum of the oscillator outputs | 29 | No click |
| Saw with the panning multiplication assigned to a local variable | 28 | 28 first stereo frames were zero |
| Saw with the same pan weights supplied as constants | 28 | No dropped frame |

The direct Saw capture aligns all 29 dropped frames to the first frame of the
new execution generation. Icosphere Pan shifts the click by 11 samples. An
earlier estimate of 7,211 samples matched the preceding cycle of the paused
tone, which repeats every 7,200 samples. Aligning the recording from the first
rendered audio resolves the ambiguity.

## Where the frame is lost

In the [Saw source](../projects/simple_sine/packages/saw/module.cpp), each of
17 voices contributes `osc * gain * 0.1` to the public stereo output.
`SineOscillator` declares one sample of output latency. The panning
multipliers read their weight input `b` one sample late so that it lines up
with the oscillator. That leaves one already authored weight per channel in
flight at the boundary.

`ProjectGraph::handle_observation_request` schedules a new execution
generation when the executable probe requirements change. It keeps the
configured generation. `plan_realtime_port_state_transition` pairs retained
port state through `StableRealtimePortStateId`. The transfer trace shows
that the oscillators keep their state. The 34 mutable, delayed pan-weight
inputs lack stable identities and receive fresh state. The first frame uses
a weight of zero on both channels. The next frame uses the correct weights.
Supplying immutable weights in the scratch package removed the reset and
left observation recompiles in place.

The [port-state rules](./sequential_port_storage_planning.md#sample-and-event-streams-share-one-lifetime-and-ownership-model)
define when anonymous state survives an authored edit. An observation
recompile needs a way to carry state within the same configured graph.
The experiment named the stereo expression, but its underlying concrete
multipliers still lacked stable port identities.

## Implementation status

`ProjectGraph` first tries to publish bindings for sources the running code
already exposes. If it needs new code, `GraphExecutor` checks the candidate's
steady layout, port state, and invocation workspace. It can replace the
Tick code and keep the existing generation when those match. The
observation frame carries its own activation number in either case.

A storage cutover now pairs anonymous bundles within the same configured
snapshot. `NodeStorage` indexes regions by owner so a move visits the
regions of the paired node. The authored identity rules still govern
transfers across different configured snapshots.

The first implementation rejected a storage cutover when its background
playback preview needed fresh Tock State. That treated a Tock cache as state
that could affect the signal. The [Tock State contract](./graph_jit_direction.md#domain-owned-state-realtime-packed-storage-or-background-sidecar)
requires the same output, coverage, and dependency propagation with a fresh
cache. The executor now allows private previews with fresh Tock State.
It still checks continuity of Tick State and retained port data.

`ExecutionGeneration::initialize_background_for_playback_preview` owns the
fresh-cache path. `adopt_background_after_drain` can move the predecessor's
cache when the successor has no preview. Observation publication has its
own request between `ProjectGraph` and `GraphExecutor`.

A preview can avoid cold cache work if it can reuse inputs or results from
the same semantic version. Another option is a read-only cache snapshot
with private writes for the candidate. This needs stable cache keys, a
lifetime for pinned entries, and an invalidation rule when the source
version changes. A snapshot of arbitrary mutable `State` would also need
a node-defined copy contract for resources that the runtime cannot copy.

Buffered Sequential recordings belong to the output, including when their
producer is observable. `PersistedDataStore` owns the committed recording.
The private playback preview reads that recording and can build derived
results. It does not publish a replacement recording. A storage cutover
keeps compatible output identities, transfers pending recording state at
the Tick boundary, and drains the predecessor's producer queues before
adopting the successor. See the [recording ownership rules](./sequential_port_storage_planning.md).

The regression tests compare sampled feedback across a storage cutover,
exercise code replacement in the active generation, and check that
removing the last probe publishes an empty frame. The tests and the
paused Saw replay still need to run against the new executable.

Preparation progress now carries an attempt ID. `ProjectGraph` assigns an
execution generation only when a candidate needs a structural cutover.
Binding-only updates and compatible Tick code changes retain the active
generation. The prediction tests use resolvable ports, and the integration
fixtures acknowledge activations while waiting for the latest preparation.
See the [runtime responsibility audit](./runtime_responsibility_audit.md) for
the remaining ownership decisions.

## Evidence

The original development bundle was
`l0n1134zj7yzjqqxgpb228jm7bvgdj0q-intravenous-dev-bundle`.
The capture files were saved on the development machine under
`/tmp/iv-saw-replay-ap4z3wt4`. They can expire. The steps to make another
editor trace are in [Replay graph editor interactions](./development_run.md#replay-graph-editor-interactions).
For audio while paused, capture the server's submitted stereo samples and
align the first rendered sample with the foreground playhead.

- `tap-saw.wav` contains the direct Saw capture.
- `paused-moving.wav` and `paused-steady.wav` contain the device comparisons.
- `requirements.stderr` lists the port-state transfer decisions.
- `cutover-trace.json` and `constant.json` contain the original and constant-weight cutovers.
