# Project presentation plugins

A presentation plugin supplies the contents of a lane. The host owns the
header, ports, input devices, viewport, playback, and project edits. The same
presentation program can generate a frame in a desktop client or in a test
that supplies the same input data.

## Run the marker example

Copy [the example project](../examples/wasm-presentation/) into a scratch
directory and open it in the development editor. The example ships a built
artifact. Drag the blue marker to move it within the lane. The project saves
its position as a fraction of the lane width.

To edit the marker, run the build watcher from the repository in the
development shell. Substitute the path to the copied example.

```sh
marker=/path/to/example/packages/marker/presentations
python3 scripts/build_presentation.py --watch "$marker/marker.cpp" "$marker/marker.wasm"
```

Change the marker ink and save the source. The build watcher replaces the WASM
file after a successful build. The lane reloads automatically. The last good
artifact stays in place when compilation fails. Use `--watch-path` to watch
additional headers or source directories.

## Declare a presentation

The provider's `iv_package.json` contains an optional native `entry` and an
optional `presentations` map. A package can supply either or both. Each key is
the presentation's ID within the package. The server publishes it as
`<package namespace>/<id>`. The key also determines the published file at
`presentations/<id>.wasm`.

```json
{
  "schema": 2,
  "namespace": "example.marker",
  "presentations": {
    "marker": {
      "title": "Marker",
      "definitionIds": ["builtin.beats"]
    }
  }
}
```

A presentation can target a public node from its package or an imported
package. The builtin package is an implicit import. Only the defining package
can supply `initialConfiguration`. The client currently chooses the first
compatible presentation by identity.

The WASM artifact contains an `iv.presentation` custom section. The first
three bytes are descriptor version (1), WASM ABI version (3), and flags
(bit 0 for lanes that share timeline overlays). An optional UTF-8 vector
path follows in a 16 by 16 icon coordinate space. The client reads this
descriptor once per digest. `IV_PRESENTATION_CLASS` emits the descriptor for
C++ guests. C guests use `IV_PRESENTATION_DESCRIPTOR`.

## Build and reload

The server loads artifacts and serves an immutable snapshot by digest through
`presentations.list` and `presentations.fetch`. It watches manifests and
published artifacts for project-local packages. Use
`IV_DEVELOPMENT_PACKAGE_PATH` when starting the server or development editor
to resolve a common package to a local checkout of the same namespace.
Multiple paths use the same separator as `IV_MODULE_SEARCH_PATH`. Common and
built-in packages read their presentation artifacts at package load.

The development tool uses Clang and wasm-ld. Set
`IV_PRESENTATION_WASM_COMPILER` and `IV_PRESENTATION_WASM_LINKER` to select
other tool paths. The Nix development shell supplies the tools. Projects can
also publish an artifact from their own toolchain. It must target core
wasm32 and implement the same exports, descriptor, and memory layout.
The runtime accepts import-free modules. Native audio package builds continue
to use LLVM bitcode.

VS Code fetches package presentations in a trusted workspace. The client
checks the artifact digest and validates frames. A replacement starts in a
separate worker and restores the lane's state before its first valid frame
replaces the running one. A failed replacement leaves the previous lane
running.

## Presentation function API

Use [`presentation.hpp`](../src/intravenous/presentation/presentation.hpp)
to author a C++ class. Declare a public default constructor and
`void present(iv::presentation::Context&)`. Use
`IV_PRESENTATION_CLASS(Type, Flags, "icon path")` to generate the C ABI entry
points and descriptor. Omit the last argument to use the host's default icon.
Omit both flags and icon with `IV_PRESENTATION_CLASS(Type)`.
Define both `save` and `restore` for
persistent state. The C SDK in
[`presentation.h`](../src/intravenous/presentation/presentation.h) provides
`IV_DEFINE_PRESENTATION` for a C state struct and callbacks.

Both forms implement the [same C ABI](../src/intravenous/presentation/abi.h).
The guest constructs one session with
`iv_create_session` and destroys it with `iv_destroy_session` on normal
teardown. Keep working data in the class or struct. The C++ `SessionBuffer`
owns guest storage through RAII. The supplied freestanding build supports
core C++ features. A project using the C++ standard library supplies its
own WASM-compatible library and allocator.

Use `Context::content_bounds` for the lane rectangle and `Context::gestures`
for the ordered input batch. Gesture and target IDs are numeric. An interrupt
appears as a cancel with ID zero. The host provides the gesture batch and one
reusable output area for each call. The SDK records spans of polylines and drag
targets there. Use `reserve_polylines` to fill the vertex storage directly or
`reuse_drawing` to keep the previous drawing.

Call `request_save` after a committed edit. The host stores the value from
the separate save callback. Use
[`state_json.h`](../src/intravenous/presentation/state_json.h) for the JSON
codec used by the marker. Live frames use the typed function API.

The example configures clangd for wasm32. If you edit the example outside
the repository, set the include path in `compile_flags.txt` to the SDK's
`src` directory.

[Prior server-built marker](historical/project_presentation_server_build_2026-11-22.md) ·
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
