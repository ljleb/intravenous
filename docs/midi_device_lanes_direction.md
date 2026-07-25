# MIDI Device Lanes Direction

## Status

This is the working design for MIDI input in the lane graph. It records
decisions made so far and is intended to be revised as implementation
proceeds.

## Goal

Allow a user to add and remove MIDI device lanes from the VS Code lane view.
A MIDI device lane is a realtime event source lane whose output is
`EventTypeId::midi`. A user selects its physical MIDI input device from a
dropdown rendered on the lane.

The MIDI input must be timestamped against the single pacing/master audio
output device so that events are placed on the realtime timeline as accurately
as possible.

## Explicit decisions

- MIDI input belongs to authored timeline lanes, not graph nodes.
- `iv::juce::midi_input()` and all of its graph-node/resource-runtime support
  have been removed.
- There may be multiple MIDI device lanes in the project, but **only one lane
  may select a physical MIDI device at a time**. Unassigned MIDI lanes are
  allowed and emit no events.
- The selected device is a global exclusive claim. A second concurrent device
  selection fails atomically and does not alter the losing lane's persisted or
  displayed selection.
- Audio output remains the sole pacing endpoint. MIDI never blocks or paces
  the task runner.
- MIDI uses a dedicated master-output-clock estimator. Do not use
  `LocalGammaTimeAligner` for MIDI timestamping. Keep it in the codebase as
  dormant infrastructure for later uses.

## Existing architecture to reuse

The repository already supplies most of the lane-level machinery:

- `AuthoredLanes` creates, reloads, persists, and reconstructs user-authored
  lanes.
- `CreatableLane` supplies creation descriptors to the VS Code client.
- Realtime event lane ports and `EventTypeId::midi` are already implemented.
- `TimelineExecution` executes realtime event lanes and lane visualization
  already publishes event activity to lane views.
- `AudioDeviceLanes` provides the model for device enumeration, selection,
  project persistence, task-runner integration, and lifecycle bridges.
- JUCE is available and already provides MIDI discovery/opening primitives via
  `juce::MidiInput`.

The relevant existing implementation locations are:

- `src/intravenous/runtime/authored_lanes.{h,cpp}`
- `src/intravenous/runtime/audio_device_lanes.{h,cpp}`
- `src/intravenous/runtime/audio_device_lane_nodes.h`
- `src/intravenous/runtime/timeline_execution_events.h`
- `src/intravenous/devices/miniaudio_device.h`

## Lane lifecycle

### Creation

Add an authored creatable lane type, tentatively named
`iv.devices.midi-input`.

The initial canonical lane UI state is:

```json
{
  "deviceId": "",
  "lastKnownName": ""
}
```

An empty `deviceId` means the lane is unassigned and produces no events.
`lastKnownName` is informational only; the stable JUCE device identifier is
authoritative.

The existing lane creation menu should offer this type as **MIDI input
device**. It must remain possible to create several unassigned MIDI device
lanes.

### Selection

The lane presentation provides a dropdown containing the current JUCE MIDI
device inventory plus an explicit unassigned option. The row should represent:

- selected and available;
- selected but unavailable/disconnected;
- unassigned;
- temporarily pending a selection request; and
- unavailable because another MIDI device lane has the global claim.

The dropdown must use a dedicated server operation rather than relying on a
generic, optimistic `setLaneUiState` round trip:

```text
setMidiDeviceLaneDevice(laneId, requestedDeviceId, expectedRevision)
```

The operation returns the authoritative lane state and revision on success.
On failure it returns a structured error, including a
`midi-device-already-claimed` error when another lane owns the claim.

### Atomic global claim

`MidiDeviceLanes` holds, under one mutex/serialized runtime operation:

```text
optional<Claim> activeClaim
Claim { lanePublicId, deviceId, deviceName, openDevice }
```

`MidiDeviceInputLaneNode` owns the corresponding RAII lease. The service is
the exclusive-claim authority; the node lifetime is the release mechanism.

For a non-empty selection request, it must:

1. Check that the target lane exists and that `expectedRevision` is current.
2. Reject if `activeClaim` belongs to a different lane.
3. Verify that the requested device still exists and open it successfully.
4. Atomically replace the lane state and global claim.
5. Notify lane state/view observers and persistence only after the commit.

For an empty selection request, it releases the claim only if the requesting
lane owns it, closes the JUCE input, and commits the unassigned lane state.

Changing the selected device on the owning lane is an atomic replacement: the
new device must open before the old claim is released. A failed replacement
therefore preserves the previously working device and lane state.

Deleting the owning lane releases and closes the claim before its timeline lane
is removed.

### Concurrent VS Code requests

Two lane views may select a device concurrently. The server is authoritative:

- exactly one request can acquire the claim;
- the other receives a failure and causes no project-state mutation;
- the losing client must dismiss only its pending visual state;
- the losing client's dropdown must retain its previous selected value;
- clients refresh from the authoritative state after a success or failure.

The client may show a pending indicator, but it must not optimistically replace
the dropdown's selected value. This avoids a rejected request changing the
previous state of the failing lane's view.

### Removal

There is existing lower-level removal support in `Timeline::remove_lane` and
the lane graph. There is not currently a public authored-lane deletion request,
authored-record deletion method, or VS Code deletion control. Add those as a
generic authored-lane capability; MIDI should use it rather than inventing a
MIDI-only deletion path.

Generic authored-lane deletion now removes authored connections and the
persistent record, then forwards `TimelineLaneBatchUpdate{.removals = ...}`
through `TimelineLaneBatchRequested`. Timeline applies the batch and emits the
authoritative refreshed lane set. MIDI uses this path unchanged.

The MIDI lane node owns its selected-device claim as an RAII lease. Timeline
owns the concrete node through `TypeErasedLaneNode`; removing the graph lane
destroys that node, so `MidiDeviceInputLaneNode`'s destructor releases and
closes its claim. No project-side MIDI pre-delete hook is needed.

## Runtime design

### `MidiDeviceLanes`

Introduce a runtime service, analogous in role to `AudioDeviceLanes`, but
without pacing responsibilities. It owns:

- the global exclusive selection claim;
- the active JUCE MIDI input handle, if any;
- MIDI device inventory and selection validation;
- a bounded timestamped MIDI queue;
- the prepared event block for the claimed lane; and
- bridges to authored-lane changes, realtime pass timing, project persistence,
  lane-view RPC, and shutdown.

It does not own a special global timeline lane. The timeline lane is the
authored lane selected by the claim.

### MIDI lane node

Add a lane node such as `MidiDeviceInputLaneNode`. It has:

- no realtime sample or event inputs;
- one `RealtimeEventLaneOutputConfig` named `midi` with
  `EventTypeId::midi`; and
- a realtime tick that requests the prepared event span for its own lane ID
  from `MidiDeviceLanes` and pushes it to `ctx.out()`.

It is a normal realtime source lane. Existing lane-graph routing, MIDI event
conversion, MIDI-to-trigger conversion, VST MIDI input, and event
visualization therefore continue to work unchanged.

Use a dedicated singleton event/builder similar to the existing audio input
block request. The returned event span is borrowed and remains valid for the
duration of the realtime pass.

### JUCE adapter

Create a runtime-level JUCE MIDI adapter, for example
`JuceMidiDeviceManager` or `JuceMidiInputDevice`, rather than placing device
ownership in the VST runtime.

Responsibilities:

- enumerate `juce::MidiInput::getAvailableDevices()`;
- return stable identifiers and display names;
- open and close the selected `juce::MidiInput`;
- implement `juce::MidiInputCallback`; and
- enqueue only fixed-size timestamped MIDI records in the callback.

Use one monotonic timestamp domain throughout. Capture a timestamp with
`std::chrono::steady_clock` when the JUCE callback receives the message unless
the platform-specific JUCE timestamp is explicitly established to share that
same domain. Do not silently mix timestamp domains.

The callback must not acquire a mutex used by realtime execution, allocate, or
perform graph work. Use a bounded SPSC queue/ring buffer; JUCE's
`AbstractFifo` is a reasonable implementation aid. Choose and document an
overflow policy, track dropped-message counts, and expose those counts in lane
diagnostics.

## Master output clock

### Why a dedicated clock is needed

`LocalGammaTimeAligner` observes generic graph callback timing and estimates
an offset through a gamma-derived update. It does not own the master
device's sample/presentation relationship, has no explicit output
latency/reservoir model, and its tests covered only simple idealized deltas. It
is not the correct authority for placing live MIDI against the pacing output.

Introduce `MasterOutputClock`, owned by the audio output path, with this
invariant:

> It estimates the monotonic real-time instant at which each timeline sample
> is or will be presented by the pacing/master audio output device.

### Observations

Extend the output-device request boundary so that a miniaudio output callback
captures a `steady_clock` timestamp at its start and passes it with the output
buffer request. `AudioDeviceLanes` already knows which timeline samples are
being drained from or appended to the output reservoir; it must associate
those sample ranges with callback observations.

The model must account for:

- actual output-device sample rate;
- requested device frame count;
- already-rendered frames in the reservoir;
- timeline block boundaries;
- backend-reported presentation/output latency where available; and
- underruns, device replacement, seek, resume, and discontinuities.

The desired mapping is an affine estimate:

```text
timelineSample ≈ slope * monotonicSeconds + offset
```

Initialize the slope from the actual output rate, then use a bounded,
outlier-resistant PLL or filtered regression to accommodate small clock drift.
Do not let a single delayed callback make a large timing correction. A
discontinuity resets or reanchors the estimate explicitly.

The mapping describes the audible/presented output stream, not merely when the
task runner happened to begin a pass. That distinction is essential when an
output reservoir means the graph is rendering ahead of playback.

### Projecting MIDI into blocks

Before a scheduled realtime pass, `MidiDeviceLanes` obtains the exact
`[blockStart, blockEnd)` range and a snapshot of `MasterOutputClock`. It maps
each queued MIDI callback timestamp to an absolute timeline sample index:

- an event in the block is emitted at its exact in-block sample offset;
- an event older than the block start is emitted at the block start and counted
  as late;
- an event at or beyond block end stays queued for a future pass;
- events are emitted in timestamp/sample order; and
- a clock reset or seek discards stale queued messages and begins a new timing
  epoch.

The realtime block quantum and operating-system callback scheduling impose an
irreducible latency floor. This design avoids adding a second, unrelated timing
estimator between incoming MIDI and the actual audio clock.

### Realtime pass notification

Do not make MIDI depend on the audio-private
`AudioDeviceLanesSetRealtimeStartIndexEvent`. Extract a general realtime-pass
notification emitted after the pacing output path has committed to a pass, for
example:

```cpp
struct RealtimePassScheduled {
    size_t start_sample;
    size_t block_size;
    double monotonic_time_seconds;
};
```

`TimelineExecution` uses it to set the realtime frontier and
`MidiDeviceLanes` uses it to prepare the claimed lane's event block. The exact
ordering must guarantee preparation before the realtime lane node ticks.

## Persistence and RPC

Persist the MIDI lane's selected device as its authored lane UI state, so it
replays with the existing `timeline.createAuthoredLane` persistence model.
The device ID is authoritative; its saved name is only diagnostic/fallback
display data.

Add RPC support for:

- fetching the current MIDI device inventory and global-claim status;
- atomically selecting/unselecting a device for a MIDI lane; and
- generic authored-lane deletion.

Selection failures must be structured, not converted into a generic stale UI
state. Useful reasons include `lane-not-found`, `revision-conflict`,
`device-not-found`, `device-open-failed`, and
`midi-device-already-claimed`.

## Completed removal of `juce::midi_input()`

The deprecated graph-node API and all runtime plumbing that existed only to
support it have been removed:

- `src/intravenous/juce/midi_input.h`;
- `JuceMidiInputSource` and `JuceMidiInputSpec`;
- MIDI input resources in `ResourceContext`;
- `JuceVstRuntimeManager::LiveMidiInput` and `create_midi_input`;
- MIDI input portions of `JuceVstRuntimeSupport` and VST runtime tick helpers;
- obsolete module examples/comments; and

`LocalGammaTimeAligner` remains available as dormant infrastructure; only its
use for MIDI time alignment is ruled out.

The new JUCE MIDI adapter should have its own build feature guard, independent
of VST hosting. MIDI support should not require a graph module to opt into VST
resources.

## Implementation sequence

1. **Complete:** add generic authored-lane deletion end to end: store,
   timeline batch, persistence, RPC, and lane-view control.
2. Add the MIDI authored lane type, its canonical UI state, and a lane
   presentation dropdown with a non-optimistic pending state.
3. Add `MidiDeviceLanes`, a fake device backend, atomic global claim handling,
   and persistence/RPC integration.
4. Add the JUCE MIDI adapter and bounded timestamp queue.
5. Introduce `MasterOutputClock` and timestamped master-output requests.
6. Connect MIDI projection to the generic realtime-pass notification.
7. Complete the new runtime MIDI device implementation without restoring a
   graph-level device-input API.

## Required tests

- creating, persisting, replaying, and deleting MIDI device lanes;
- unassigned and unavailable-device lane behavior;
- successful selection, replacement, unselection, and deletion release;
- two concurrent allocations: exactly one success, no mutation of the losing
  lane, and correct client-visible error;
- queue ordering, overflow accounting, and no-event behavior without a claim;
- MIDI event offsets at block start, middle, end, late, and future boundaries;
- clock stability under callback jitter and small output-clock drift;
- discontinuities, output-device replacement, seek, resume, and underrun
  reset behavior; and
- removal of the deprecated graph-node API from public headers, runtime
  resources, and module compilation paths.
