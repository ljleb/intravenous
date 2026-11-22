# Server-built presentation checkpoint

Preserved on 2026-11-22 from [the presentation guide](../project_presentation_plugins.md)
and [the implementation fronts](../implementation_fronts_direction.md).
The replacement uses package-declared, externally built WASM artifacts and
explicit guest session lifetime. The snapshot documents the server-built
marker before that change.

# Project presentation plugins

A presentation plugin supplies the contents of a lane. The host owns the
header, ports, input devices, viewport, playback, and project edits. The same
presentation program can generate a frame in a desktop client or in a test
that supplies the same input data.

## Run the marker example

Copy [the example project](../examples/wasm-presentation/) into a scratch
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

## Presentation function API

Use [`presentation.h`](../src/intravenous/presentation/presentation.h) and
define a typed session struct. Bind the struct and callbacks with
`IV_DEFINE_PRESENTATION`. The host allocates the struct once per worker
instance and passes it to each presentation call. Use `iv_session_allocate`
and `iv_session_free` for dynamic session data. Destroying the worker releases
its WASM memory.

Call `iv_content_bounds` for the lane's content rectangle and
`iv_gesture_inputs` for the ordered input batch. Gestures and drag targets
use numeric IDs. An interrupt appears in that sequence as a cancel with ID
zero. `iv_can_edit` reports the host's editability decision.

Use `iv_draw_polylines` to record a span of polylines. Use
`iv_reserve_polylines` to fill the frame's vertices directly. Register drag
targets with `iv_register_drag_targets`. The SDK records these operations
inside WASM. The host prepares one reusable output area for the call, then
validates the batch and transfers its packed coordinates to the renderer.
Use `iv_reuse_drawing` when the previous drawing still applies. Targets can
change independently of that decision.

The host calls restore when it opens the session or applies saved state.
Call `iv_request_save` after a committed edit to ask the host to serialize
the session through the separate save callback. Use
[`state_json.h`](../src/intravenous/presentation/state_json.h) for the JSON
codec used by the marker. Live frames use the typed function API.

The example configures clangd for `wasm32-unknown-unknown`. If you copy the
example outside this repository and edit it there, set the include path in
`compile_flags.txt` to the SDK's `src` directory. With Nix's wrapped clangd,
use the repository's `--query-driver` setting to keep native C headers out
of the WASM include search path.

This ABI 2 marker milestone exercises drawing, hit targets, drag replay,
session allocation, and save and restore. The next built-in migrations will
extend it through typed functions for the data and operations the lanes use.
Package manifests that request observations or timeline snapping report an
unsupported capability until those functions are available.
The [existing built-in contract](../vscode/client/src/presentationPlugin.ts)
still serves Beats, Automation Curve, and Click Detector during the migration.

[Earlier JSON frame ABI](historical/project_presentation_json_frame_abi_2026-11-22.md) ·
[Earlier JavaScript pilot](historical/project_presentation_javascript_pilot_2026-11-22.md)

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

Run the scenario with the [headless client](./development_run.md#replay-graph-editor-interactions).
The report includes frames, accepted revisions, and the last snapshot.
The scenario keeps presentation state in memory. It routes node edits
through the server and uses the snapshot data for layout and observations.

```sh
node build-release/vscode/client/dist/headlessClient.js present \
  --server "$IV_TEST_SERVER" --project "$IV_TEST_PROJECT" \
  --scenario /path/to/scenario.json
```

## Former implementation checkpoint

**WASM marker implementation checkpoint (2026-11-22).**
`PackageWatcherService` refreshes `PresentationArtifacts` from discovered
packages and published node definitions on its worker. It builds changed
freestanding C source with Clang and wasm-ld. Packages with no native entry
skip the native build. `presentations.list` and `presentations.fetch` read
from the published catalog, and VS Code and the headless client verify each
downloaded artifact before executing it through the same ABI adapter.
[Marker project and ABI guide](project_presentation_plugins.md)

The Nix native compiler wrapper adds host-specific hardening flags that
the wasm32 backend rejects. The presentation build uses a separately
configured compiler and linker. It does not inherit the DSP build flags.
The specialized built-in migrations and the explicit lane selector are
the next presentation milestone.

**Batched function ABI checkpoint (2026-11-22).** Presentation authors call
`presentation.h` functions. The guest SDK records span-based drawing and
hit-target operations during one call. Both hosts reserve a session struct
and reusable output storage at worker startup. The renderer receives packed
vertices. The session can keep the previous drawing while the host updates
the hit targets. JSON is confined to the save and restore callbacks.

The marker exercises this boundary. The following observations from the
built-in lanes govern the next migration:

- `createBeatsPresentation` implements `openSeekReader` as a synchronous
  reader over the displayed `BeatGrid`. `usePlayheadSeek` asks it about
  windows outside the viewport during a drag. A worker cannot supply that
  answer synchronously without blocking pointer motion. Move scheduling and
  reuse of seek windows into the host. Capture the displayed grid when a
  drag starts and use that same grid for the entire gesture. Verify the
  detent behavior when an outside-window result is pending before porting
  Beats.
- `createClickDetectorPresentation` reads the retained ready spans of its
  `event-timestamps` observation while a newer window loads. Keep window
  retention in the host and give the presentation a borrowed typed span of
  sample indices. The 5,000-event regression already exceeds the marker's
  polyline descriptor budget. Use a batch primitive for the vertical
  markers and retain each visible event, including equal-time occurrences.
- `createAutomationCurvePresentation` reads accepted control points from
  `PresentationSnapshot.node.arguments`. The host currently obtains that
  field from `NodeConstructionArguments` JSON. Publish a typed control-point
  view on accepted changes before porting the editor. Keep local gesture
  drafts and selection in the presentation session. The host already keeps
  the min and max spans coherent. Borrow them together, with exact sample
  indices, and keep the same windows allocated across viewport-only updates.
  The editor also receives revision acknowledgements and supplies a cursor
  destination on release. Preserve their ordered delivery independently of
  whether the rendering host displays the resulting frame.

The two workers currently embed `createWasmPresentation` as source text.
That forces packet decoding to live inside one self-contained function.
Switch to bundled worker entry points before adding the observation and edit
interfaces, so each part of the ABI can have its own implementation and tests.

The generic presentation and the current built-in lanes keep their
existing host behavior while these dependencies are exercised.
[Function API and marker](project_presentation_plugins.md)


## Former Click Detector replay report

From [the development run guide](../development_run.md). Viewport
observations now return bounded trigger counts.

If the recorded viewport observes a Click Detector, `recordedEvents` lists
the sample indices returned by that port. `playback.outputProfile` contains
the stereo samples around graph switches. To compare detector events with
switches, use the indices in the same replay. The recording starts from the
saved graph. Changes by other panels and browser pointer handling need a live
graph editor test.

