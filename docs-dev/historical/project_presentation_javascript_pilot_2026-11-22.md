# Project presentation JavaScript pilot

_Superseded 2026-11-22. Preserved from the project presentation plugin
guide when the server began building and publishing WebAssembly. See
[Project presentation plugins](../project_presentation_plugins.md)._

## Add a presentation

In a trusted Intravenous workspace, put the manifest and its matching
JavaScript file in `packages/<package>/presentations/`. A manifest describes
the plugin and names the node definitions that it presents. The graph editor
reloads the catalog when those files change. The native package build skips
this directory.

This example uses the package directory `marker` and attaches a draggable
marker to Beats. Use a stable plugin ID that is unique within the project.
Change `definitionIds` to attach the presentation to your own node. A
package directory that contains only presentations works without an
`iv_package.json` or a C++ entry file in this pilot.

`packages/marker/presentations/marker.json`

```json
{
  "version": 2,
  "id": "example.marker",
  "title": "Marker",
  "definitionIds": ["builtin.beats"],
  "icon": "M5 2v12m4-12v12",
  "timeline": true
}
```

`packages/marker/presentations/marker.js`

```js
let draft;

function present(view, inputs) {
    const { width, height } = view.viewport;
    for (const input of inputs) {
        if (input.type !== "gesture" || input.id !== "marker") continue;
        if (input.phase === "move" || input.phase === "finish") {
            draft = Math.max(0, Math.min(1, input.point.x / width));
        }
        if (input.phase === "cancel") draft = undefined;
    }
    const x = width * (draft ?? view.state?.position ?? 0.5);
    const committed = inputs.some(input => input.type === "gesture"
        && input.id === "marker" && input.phase === "finish");
    const commands = committed && draft !== undefined
        ? [{ type: "state", value: { position: draft } }]
        : [];
    if (commands.length) draft = undefined;

    return {
        status: "Drag the marker",
        marks: [
            { type: "polyline", id: "line", ink: "output", points: [{ x, y: 0 }, { x, y: height }] },
            { type: "rect", id: "handle", ink: "output", x: x - 4, y: 8, width: 8, height: 8 },
        ],
        targets: [{ id: "marker", x: x - 8, y: 0, width: 16, height }],
        commands,
    };
}
```

The optional `icon` is a path in the host's 16 by 16 header icon. Use
`timeline: true` for a lane that follows project time. Built-in plugin IDs
use the `builtin.` prefix. When multiple project plugins claim a definition,
the first plugin ID in sorted order supplies its presentation.
Set `initialConfiguration` to a configuration expression when the node picker
needs default construction arguments. The host submits that expression
through the normal node creation route.

The VS Code host runs each project script on demand in a web worker. Supply
one self-contained script with a top-level `present` function. Keep frames
bounded. The host validates them before drawing or dispatching commands.
The portable contract is defined in
[`presentationPlugin.ts`](../vscode/client/src/presentationPlugin.ts).

