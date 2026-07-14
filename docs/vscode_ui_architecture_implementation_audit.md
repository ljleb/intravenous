# VS Code UI Architecture: Implementation Audit and Priorities

**Audit basis:** current `vscode/client/src` implementation, reviewed 2026-07-14.
This records implemented behavior, not merely runtime capability. It does not
replace the intended direction in `vscode_ui_architecture_direction.md`.

## Implemented now

### Platform and integration foundation

- The extension host owns the server/socket session, notification routing,
  graph/lane panel lifecycle, lane-view registry, commands, and editor
  highlight integration.
- Webviews communicate with the extension host; they do not make JSON-RPC calls
  directly.
- The extension is TypeScript and uses `esbuild`; `tsyringe` is already used
  for the session factory.
- Playback actions, project save/autosave, lane creation, and lane opening are
  available as VS Code commands. Playback has a `shift+space` binding.
- A status-bar item reports module-rebuild progress/failure. This is not yet a
  general transport-status surface.

### Live graph

- A persistent live-graph sidebar is registered in the Intravenous activity
  container and receives incremental graph updates.
- It shows logical nodes, merged-node members, port groups, current values,
  defaults/ranges where applicable, authored/effective state summaries, and
  supported sample/event input/output state transitions.
- It supports input value editing and reset/default actions, including the
  state mutations routed through the existing graph JSON-RPC methods.
- Source editor selection queries/reorders the visible graph and drives source
  highlighting.
- The existing graph webview uses VS Code theme variables extensively.

### Lane panels

- Multiple lane webview panels can be opened, revived by VS Code, registered
  with the runtime using stable client-generated IDs, and independently closed.
- The runtime lane-view open/update/close requests and lane-view/content
  notifications are wired through to the panels.
- Panels render a timeline ruler, a vertical lane stack, transport/playhead
  position, lane virtualization/viewport updates, and currently visible local
  connections.
- Lane-kind-specific rendering exists for the generated beat-trigger plugin and
  realtime meter/event presentation. Lane UI state can be persisted through
  `timeline.setLaneUiState`.
- The lane UI already uses VS Code theme variables and has a dense timeline
  direction, but it is still a hand-written DOM application.

### Explicitly not implemented

- React webview applications, separate webview bundles, typed webview message
  contracts, and typed UI stores/hooks. `lanesViewProvider.ts` currently has
  `// @ts-nocheck`, and both major webviews generate large HTML/DOM scripts.
- A user-editable lane query UI. A `laneQuery` field is carried in view state,
  but the panel has no query-bar interaction that changes it.
- The separate visual-settings query/language and all of its policies
  (sync, cable targets/peers, header fields, metadata visibility, and noise
  suppression).
- A defined default lane-header/track anatomy and the requested visual design
  specification.
- Authored project lane colors and lane-view context actions to edit them.
- Correct cross-view cable semantics: current panels serialize only endpoints
  among their local lanes, rather than rendering local fragments plus explicit
  off-view continuations.
- Bidirectional graph/lane navigation: neither graph-to-lane reveal/filter nor
  lane-to-graph/source reveal is wired as a user action.
- Named, ordered, persistently switchable lane workspaces, restoration of the
  last active workspace, and broader working-context restore.
- Native context-menu/panel-title command contributions and selection-sensitive
  lane commands. Current commands are global and mostly only open/create or
  playback/project actions.

## Priority order

### P0 — Define the stable UI contract before more bespoke UI work

1. Produce the visual design spec required by the direction document: the
   default live-graph row anatomy and default lane header/track anatomy, with
   theme-token and accessibility rules.
2. Specify the lane-view model in one typed contract: stable ID, lane query,
   visual-settings query, viewport, presentation policy, selected lane, and
   cable continuation data. Confirm which fields existing open/update payloads
   can carry; enrich those payloads rather than adding routes unless necessary.
3. Implement the separate lane-query and visual-settings controls against that
   contract. This is the smallest product slice that makes user-maintained lane
   views real rather than static snapshots.

### P1 — Restore the core navigation and lane workspace loop

4. Add lane-to-source and lane-to-live-graph reveal from lane context actions;
   make source selection update/filter the focused lane view. Use client-side
   mapping over existing graph/lane data first.
5. Render cross-view cable fragments with clear off-view continuation markers.
   Honor cable-target/peer behavior once the visual-settings model exists.
6. Add authored lane color and lane context actions, then surface the color
   consistently but modestly across views.

### P2 — Make the implementation architecture match the direction

7. Replace the large hand-written webview HTML scripts with React + TypeScript
   applications, separate `esbuild` entrypoints, and explicit typed host/webview
   message contracts. Migrate one surface at a time, starting with lanes.
8. Extract extension-host services (lane registry, focused-view/reveal service,
   transport/status coordination) behind injected interfaces; keep React
   components prop/hook driven rather than using a service locator.
9. Move high-frequency and selected-view actions to native VS Code menus,
   title actions, and keybindings. Add a real transport status indicator to the
   status bar; retain commands as the primary action surface.

### P3 — Saved working sessions

10. Implement named, ordered lane workspaces, last-active restoration,
    dropdown/cycling, and exclusive panel-set switching. Treat broader editor
    context restoration as a best-effort follow-up constrained by VS Code.

## Recommended first implementation slice

Take P0 items 2–3 together: add typed lane-view state and UI controls for the
lane query plus visual-settings query, initially with the current default
header anatomy encoded as settings. This establishes the enduring model needed
by navigation, cable policy, colors, workspaces, and the React migration while
avoiding new JSON-RPC routes prematurely.
