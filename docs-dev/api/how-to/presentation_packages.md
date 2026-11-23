# Build, reload, and test a presentation package

## Publish a built artifact

Add a `presentations` map to `iv_package.json`. Each key is the presentation's
ID, and its artifact lives at `presentations/<id>.wasm` relative to the
package root. Use `definitionIds` to select public nodes from the package or
its imports. The builtin package is implicitly imported. A package can
also declare a native `entry`.

Build a C or C++ file in the development shell. Substitute your source and
artifact paths.

```sh
python3 scripts/build_presentation.py package/view.cpp package/presentations/view.wasm
```

Use `--watch` to rebuild on source changes. Repeat `--watch-path` for
headers or other directories you want the watcher to observe. The tool uses
`IV_PRESENTATION_WASM_COMPILER` and `IV_PRESENTATION_WASM_LINKER` when set.
Publish a core wasm32 module with the required descriptor and exports if
you use your own build tool.
[Build tool](../../../scripts/build_presentation.py) · [C ABI contract](../reference/presentation_c_abi.md)

## Reload the artifact you are developing

Put the package in your project's source tree. The server watches the
manifest and built artifact for direct project packages. For a common
package, set `IV_DEVELOPMENT_PACKAGE_PATH` to the local checkout before
launching. Separate multiple roots as for `IV_MODULE_SEARCH_PATH`.
Released package artifacts are loaded when the package loads.

Publish by writing a temporary file and atomically renaming it over the
artifact after a successful build. The build tool does this. In VS Code,
use a trusted workspace to load project presentation artifacts. If a
reload fails, check Intravenous output for the catalog or worker error.

## Bind a recorded trigger output

Declare the observation in the presentation entry. The guest looks up the
`id` in the observation span it receives each frame.

```json
{
  "title": "Click Detector",
  "definitionIds": ["builtin.click_detector"],
  "observations": [
    {"id": "clicks", "port": "clicks", "direction": "output", "operation": "trigger-bins"}
  ]
}
```

Use a buffered Tick trigger output. If the observation is unavailable,
check the node ID, the port name, and the output declaration.
[Shipped binding](../../../src/intravenous/builtin_packages/builtin/iv_package.json)

## Bind a waveform input or output

Declare `operation: "waveform"`, choose an input or output, and request
between 2 and 1,024 points in the visible view. Use `whenConnected: true`
when an unconnected port needs no subscription. Give related bindings the
same `group` when they must agree on the selected observation windows. Use
`completeOnly: true` to keep the previous complete view while a replacement
is pending.

```json
{"id":"value","port":"value","direction":"output","operation":"waveform","points":512}
```

In C++, use `frame.observed_waveform(id)` to access its Regions. Draw each
Region independently and clip to its half-open `begin` and `end` offsets.
The view can carry retained data during refresh. The host owns requests,
prefetching, and selection of retained windows.
[Waveform records](../reference/presentation_c_abi.md#host-context-and-borrowed-views) ·
[Placement example](../../../examples/wasm-presentation/packages/marker/presentations/placement.cpp)

## Test a presentation scenario

Use a scratch copy of the project. Set `IV_TEST_SERVER` to the matching
server executable and `IV_TEST_PROJECT` to the project path. Build the VS
Code client bundle before running these commands.

```sh
node build-release/vscode/client/dist/headlessClient.js present \
  --server "$IV_TEST_SERVER" --project "$IV_TEST_PROJECT" \
  --scenario "$IV_TEST_PROJECT/marker-scenario.json"
```

The marker scenario drags the marker to 30 percent of the lane width. For
your own scenario, supply a `snapshot` and an `inputs` array. Put the node's
address in `snapshot.node.address` and a relative time `atMs` on each
input. Use `inspect` to find the address, and provide `expanded` if
you need a node inside an expanded module. The headless runner reads
accepted node arguments from the server and takes viewport and observation
data from the scenario. Inspect the returned frames and final state.
[Example scenario](../../../examples/wasm-presentation/marker-scenario.json) ·
[Headless command setup](../../development_run.md#replay-graph-editor-interactions)
