# Project presentation plugins

A presentation plugin supplies the contents of a lane. The host owns the
header, ports, input devices, viewport, playback, and project edits. The same
presentation program can generate a frame in a desktop client or in a test
that supplies the same input data.

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
  "version": 1,
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

## Frame and input contract

- `present(snapshot, inputs)` returns a frame synchronously. The snapshot
  includes accepted construction arguments, port metadata, the viewport,
  display mode, edit availability, declared observations, and the lane's
  saved JSON state.
- Coordinates are measured in logical pixels from the top left of the
  content area. Sample indices are decimal strings. A frame may include
  polylines, rectangles, text, and rectangular hit targets. Keep IDs stable
  when geometry changes. Return `seekMarks` to opt into the host's playhead
  gesture and provide snap targets from the current viewport.
- A settings panel can contain rows of labels, knobs, choices, and actions.
  The host sends control input as `preview`, `commit`, and `finish`.
  Hit targets receive `begin`, relative `move`, `modifiers`, `finish`,
  and `cancel` with the current modifier keys.
- Commands may edit the lane's own node through a supported serialized node
  event, finish an edit gesture, or update the lane's own JSON state. The
  host accepts commands in response to user input. The server validates node
  events and the next snapshots report accepted arguments, pending edits,
  and any edit error.
  Set `replacePending: true` on a gesture edit only when it supplies a full
  replacement for earlier previews of that event in the same gesture. The
  host can pace those updates and deliver only the newest pending value.
  Finishing the gesture flushes that value before closing the edit.
  Store persistent state via the host. A worker can be discarded when its
  lane has been off screen for a while.
- A manifest can declare continuous waveform observations as
  `{ "id": "wave", "direction": "output", "port": "value", "points": 256 }`
  entries in `observations`. The host resolves ports on the lane's node and
  picks the observation tiers. Read the result and its visible spans from
  `snapshot.observations.wave`. The result carries the observation status,
  sample points, and coverage regions. A missing observation is `null`.

The VS Code host runs each project script on demand in a web worker. Supply
one self-contained script with a top-level `present` function. Keep frames
bounded. The host validates them before drawing or dispatching commands.
The portable contract is defined in
[`presentationPlugin.ts`](../vscode/client/src/presentationPlugin.ts).

The pilot supports local project artifacts, continuous waveform observations,
and visible snap marks for project scripts. Beats also keeps its built-in
seek reader for extrapolating past the viewport. Automation Curve remains on
its current editor while selection, scroll sensitivity, and release cursor
placement are worked through the portable gesture contract.
