# JSON frame presentation pilot

_Superseded 2026-11-22. Preserved from the presentation guide before the
function API replaced the per-frame JSON request and reply. See
[Project presentation plugins](../project_presentation_plugins.md)._

# Project presentation plugins

A presentation plugin supplies the contents of a lane. The host owns the
header, ports, input devices, viewport, playback, and project edits. The same
presentation program can generate a frame in a desktop client or in a test
that supplies the same input data.

## Run the marker example

Copy [the example project](../../examples/wasm-presentation/) into a scratch
directory and open it in the development editor. Drag the blue marker to
move it within the lane. The project saves its position as a fraction of the
lane width. Change the marker ink in `packages/marker/presentations/marker.c`
and save. The server builds the changed presentation and the editor reloads
the lane.

## Add a presentation

Declare the provider package in `iv_package.json`. A package with only
presentations can omit `entry`. Place a manifest and a freestanding C source
file with the same stem in `packages/<package>/presentations/`. The manifest
uses its filename stem as `id`. The server publishes the identity as
`<package namespace>/<id>`. Each target in `definitionIds` must be a public
definition from this package or an imported package. The builtin package
is an implicit import. Only the defining package can supply
`initialConfiguration`.

The optional `icon` is a path in the host's 16 by 16 header icon. Use
`timeline: true` for a lane that follows project time. In this milestone
the client chooses the first compatible package presentation by identity.
A later milestone adds explicit lane choices.

The package service builds the source with freestanding Clang and
`wasm-ld`. The Nix development shell supplies both. Other development
environments can set `IV_PRESENTATION_WASM_COMPILER` and
`IV_PRESENTATION_WASM_LINKER` in CMake. Presentation source edits publish
a new artifact without rebuilding the audio graph. A failed source build
keeps the last successful artifact when its manifest and targets still match.

Clients with project read access call `presentations.list` for the catalog.
It reports `ready` after the first discovery and lists the artifact format,
ABI version, data schema version, and SHA-256 digest. Clients call
`presentations.fetch` with a compatible digest and verify the base64-encoded
bytes against it. VS Code only loads package presentations in a trusted
workspace. Each host owns the worker, validates its frames, and routes its
commands through the ordinary project edit path.

### WebAssembly ABI version 1

Use core WebAssembly with 32-bit offsets and no imports. Export `memory`,
`iv_alloc(size)`, `iv_free(offset)`, and
`iv_present(request_offset, request_length, response_offset, response_capacity)`.
The first two functions allow the host to reserve its buffers in module
memory. A successful allocation returns a nonzero offset. The buffers must
be disjoint. Offsets and sizes are WebAssembly i32 values. The host may reuse
buffers across calls and allocate a larger request buffer when needed.

In C, include [`intravenous/presentation/abi.h`](../../src/intravenous/presentation/abi.h)
and implement `iv_present` with buffer pointers, `uint32_t` byte counts, and
an `int32_t` result. The package compiler links the allocator into each
module. The authors' pointers compile to the same i32 offsets that the host
uses.

Use [`intravenous/presentation/frame.h`](../../src/intravenous/presentation/frame.h)
to write a frame into the response buffer. The writer handles JSON escaping
and structure, tracks the buffer size, and returns a negative result if the
frame cannot fit. It also provides helpers for polylines and absolute drag
targets.

The example configures clangd to parse its source for
`wasm32-unknown-unknown`. If you copy the example outside this repository
and edit it there, change the include path in its `compile_flags.txt` to the
SDK's `src` directory. With Nix's wrapped clangd, use the repository's
`--query-driver` setting so the wrapper leaves the native C headers out of
the WASM include search path.

The request is UTF-8 JSON with `snapshot` and `inputs` fields. The response is
a UTF-8 JSON frame. Return its positive byte length or a negative error code.
The current hosts accept at most 1 MiB in each direction per invocation. The
server limits the built module to 8 MiB of linear memory and 256 KiB of
artifact bytes. A failed worker invocation discards its instance.

[Earlier JavaScript pilot](project_presentation_javascript_pilot_2026-11-22.md)

## Frame and input contract

Version 2 supplies the time axis in samples.
[Version 1 time coordinates](project_presentation_time_v1_2026-11-22.md)

- `present(snapshot, inputs)` returns a frame synchronously. The snapshot
  includes accepted construction arguments, port metadata, the viewport,
  display mode, edit availability, declared observations, and the lane's
  saved JSON state. `clockMs` gives the host's current monotonic time in
  milliseconds. Use it with `wakeAfterMs` in a frame to request another
  update during an edit, even when the user stops moving the pointer.
- When `viewport.time` is present, `beginSample` is a signed decimal index.
  `beginFraction` is the fraction of a sample after that index.
  `spanSamples` is the forward distance across the viewport in samples.
  At one lap, the span is `2^64` and both edges represent the same position.
  `sampleRate` gives the rate for converting distances to seconds.
- Coordinates are measured in logical pixels from the top left of the
  content area. Sample indices are decimal strings. A frame may include
  polylines, rectangles, circles, text, and rectangular hit targets. Keep
  IDs stable when geometry changes. Return `seekMarks` to use the host's
  playhead gesture with the snap targets in the current viewport. Set
  `timelineSnapping: true` in a timeline manifest to receive the current
  snap marks and playhead position when a hit target's gesture starts.
- A settings panel can contain rows of labels, knobs, choices, and actions.
  A context menu uses those same controls. A hit target can ask for
  primary or secondary drag, a context menu, and wheel input during a
  drag. It receives `begin`, `move`, `modifiers`, `finish`, and `cancel`
  with the current modifier keys. The default relative drag can use
  pointer lock. Use `drag: "absolute"` for a rectangle selection.
  Control input arrives as `preview`, `commit`, and `finish`.
  Use `itemEditing` to receive item shortcuts while the lane content has
  focus, and `valueNavigation` to receive lane-local pan, zoom, and reset.
- A target with `cursorReturn: true` can return `drag.cursor` in its release
  frame. The host resolves that lane coordinate against the visible area
  and moves the cursor when the platform supports it. The host owns the
  native pointer lock, capture, and release. A worker frame must arrive
  within the same cursor return deadline as a built-in frame.
- Commands may edit the lane's own node through a supported serialized node
  event, finish an edit gesture, or update the lane's own JSON state. The
  host accepts commands in response to user input. The server validates node
  events and the next snapshots report accepted arguments, pending edits,
  and any edit error.
  Set `replacePending: true` on a gesture edit only when it supplies a full
  replacement for earlier previews of that event in the same gesture. The
  host can pace those updates and deliver only the newest pending value.
  Finishing the gesture flushes that value before closing the edit.
  For edits that depend on the result of an earlier edit, give each
  command a unique `requestId`. The host sends back `editResult` with that
  ID and the accepted revision or error. Use `replacePending` for
  replaceable previews and `requestId` for edits that require a reply.
  On history navigation or disconnect, the host sends `interrupt`.
  Store persistent state as a JSON object through the host. A worker can
  be discarded when its lane has been off screen for a while.
- A manifest can declare continuous waveform observations as
  `{ "id": "wave", "direction": "output", "port": "value", "points": 256 }`
  entries in `observations`. The host resolves ports on the lane's node and
  picks the observation tiers. Read the result and its visible spans from
  `snapshot.observations.wave`. The result carries the observation status,
  sample points, and coverage regions. A missing observation is `null`.
  Use `whenConnected` to observe only a connected port. Use
  `completeOnly` if a partial result should leave the previous complete
  spans in view.
- Use the same `group` name on observations that feed one plot. The host
  updates the group together when one snapshot answers each visible
  range. A ready result with empty coverage counts as an answer.
  Without a group name, each observation updates independently.
- The host can split one viewport observation into several server requests.
  A span's `begin` and `end` are offsets from the start of the viewport's
  observation window, in project samples. The interval includes `begin`.
  It excludes `end`. `windowOffset` gives the offset of the result window
  from that same start. It can be negative when a retained window overlaps
  the left edge. The observation window starts at `beginSample`. For a
  full lap with a nonzero `beginFraction`, it starts at the next sample.
- An output observation can set `"operation": "event-timestamps"` to read
  a concrete node's buffered sequential event output. Omit `points` for this
  operation. Each ready span's `result.eventIndices` contains every signed
  decimal sample index in the observed range, including equal-time events.
  The host requests the visible range and keeps the previous complete window
  available while the next one loads. The frame can include one additional
  mark per observed event.

The VS Code and headless hosts run package WebAssembly in workers. Keep frames
bounded. The host validates them before drawing or dispatching commands.
The portable frame contract is defined in
[`presentationPlugin.ts`](../../vscode/client/src/presentationPlugin.ts).

The built-in Automation Curve uses this contract for authoring, selection,
lane navigation, and the connected min and max traces. Beats also has a
built-in seek reader for extrapolating past the viewport. The
[rendering review](project_presentation_rendering_review.md#automation-curve-during-the-pilot)
records the earlier pilot status.

A future presentation API extension will support typed transfers between
lanes, starting with Automation Curve control points. See
[Copy, cut, and paste between lanes](../node_interaction_and_presentation_communication_direction.md#copy-cut-and-paste-between-lanes).

## Test a presentation scenario

The headless client fetches the same package artifacts as VS Code. It validates
frames and uses the shared edit queue to send node events. Use a copy of a
trusted project because the server can save edits. The example project includes
`marker-scenario.json`, which selects the marker by its published presentation
ID and drags it to 30 percent of the lane.

Create a scenario JSON file with a `snapshot` and an `inputs` array. Use
`snapshot.node.address` to select a node in the test project. Use `inspect`
to find the address. Supply `expanded` alongside `snapshot` if the target is
inside an expanded module. The runner reads accepted arguments from that
node and uses the viewport and observations in the scenario file.

Each entry in `inputs` contains `atMs` and `value`. The time is relative to
the start of the scenario. The value is a `PresentationInput`. For example,
these two inputs move and release the Beats BPM knob.

```json
[
  { "atMs": 20, "value": { "type": "control", "id": "bpm", "phase": "preview", "value": 150 } },
  { "atMs": 80, "value": { "type": "control", "id": "bpm", "phase": "finish" } }
]
```

Run the scenario with the [headless client](../development_run.md#replay-graph-editor-interactions).
The report includes frames, accepted revisions, and the last snapshot.
The scenario keeps presentation state in memory. It routes node edits
through the server and uses the snapshot data for layout and observations.

```sh
node build-release/vscode/client/dist/headlessClient.js present \
  --server "$IV_TEST_SERVER" --project "$IV_TEST_PROJECT" \
  --scenario /path/to/scenario.json
```
