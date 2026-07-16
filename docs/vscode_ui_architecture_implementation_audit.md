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
- A per-panel editable lane query control, with debounced updates through the
  lane-view RPC path and VS Code webview-state restoration. It does not yet
  provide inline errors, match counts, clear/reset, completion, or a separate
  visual-settings query.
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

## Functional-first delivery phase

This phase should deliberately establish the day-to-day lane-view interaction
model before a React rewrite, saved workspaces, or a comprehensive visual
settings language. It should have modest UI: a compact, keyboard-accessible
control region and native/context actions where they are better than permanent
chrome.

### 1. Collapsible lane-view controls — first

- Add a top-of-panel **View controls** disclosure. When collapsed it occupies
  only one compact button/row; it must not leave an empty toolbar band.
- Put the lane query text field in that region. Restore its value with the
  panel state, submit on explicit Enter/apply and on a short debounced edit,
  show parse/query errors inline, and keep the last valid results visible on
  an invalid query.
- Include a compact matching-lane count and a clear/reset action. These make
  the query usable without committing to a sophisticated query editor.
- Reserve the second field in this same region for the visual-settings query,
  but it may initially be hidden behind an “Advanced view settings” disclosure
  until there is functional behavior to expose. Do not conflate it with the
  lane selection query.
- Preserve the collapsed/expanded state per panel. It is presentation state,
  rather than a new project-authored view property.

### 2. Lane selection and inspection primitives

- Make a lane row selectable with clear focused/selected state and keyboard
  navigation. All subsequent actions should operate on that selection.
- Add a small, non-sticky detail affordance (context menu or temporary
  inspector/popover) exposing lane ID, type, relevant metadata, and current
  connections. Avoid permanently expanding every track header.
- Provide **reveal source** and **reveal in live graph** for a selected lane as
  soon as the available mapping permits. This is a useful functional action
  even before bidirectional navigation is fully polished.
- Add copy actions for lane ID and a query that isolates the selected lane;
  they are inexpensive but make manual view composition practical.

### 3. Connections — the main functional slice

Start with a connection inspector/list rather than canvas cable editing:

- From a selected source lane, choose **Connect output…**; show only compatible
  target lane inputs, including port name/index and kind/channel compatibility.
- From a target input, choose **Connect from…**; show compatible sources.
- Allow adding any compatible contributor and provide **Disconnect** for an
  existing edge. The lane graph is a DAG: inputs may have multiple
  contributors and outputs may fan out; only cycles are forbidden.
- Show connection direction, source/target, port, and an unambiguous status in
  the existing Connections section. Selecting a connection should select or
  reveal either endpoint locally where possible.
- If the peer is filtered out, retain a compact endpoint/continuation record;
  do not silently omit the connection. This is the functional precursor to
  richer cable fragments.
- Report rejected/incompatible or stale operations clearly and refresh from the
  authoritative runtime response/notification rather than maintaining an
  optimistic project graph indefinitely.

The current client only displays connections returned by a lane-view result;
it has no connect/disconnect RPC wrapper or webview action. Runtime project
persistence recognizes `timeline.connectLanes` and
`timeline.connectAuthoredLanes`, but the current socket request dispatcher does
not expose corresponding JSON-RPC methods. Therefore this slice needs a small,
typed mutation contract in addition to UI work. Define it with its response
and validation data first; do not make the UI infer compatibility from display
metadata alone.

### 4. Small high-value support actions that fit this phase

- Lane context actions for rename/color, **only if** the project-state mutation
  model is ready; otherwise defer rather than creating view-local fake state.
- Native commands for focus next/previous lane, expand/collapse View controls,
  focus query, and disconnect selected connection. Add keybindings after the
  behavior settles.
- A compact panel title/status summary: query match count, selected lane, and
  playback position. It must not become the rejected large transport strip.
- Persist query, collapse state, scroll/viewport, and selected lane in VS Code
  webview state so restored panels resume useful work.

### Explicit exclusions from this phase

- React migration and the broad visual redesign.
- Named workspaces and desktop-style workspace restoration.
- Advanced visual-settings semantics (sync groups, peer-view policy, metadata
  noise suppression) beyond reserving the separate field/model.
- Canvas-style cable dragging, cross-panel overlays, and elaborate cable art.
- Bulk routing, multi-select routing, and automatic layout.

## Longer-term priority order

### P0 — Stabilize the model after the functional phase

1. Produce the visual design spec required by the direction document: the
   default live-graph row anatomy and default lane header/track anatomy, with
   theme-token and accessibility rules.
2. Consolidate the functional-phase lane-view state into one typed contract:
   stable ID, lane query, visual-settings query, viewport, selected lane, and
   cable continuation data. Enrich existing payloads where possible.

### P1 — Restore the core navigation and lane workspace loop

3. Complete source-selection filtering for the focused lane view and polish
   lane-to-source/live-graph reveal. Use client-side mapping over existing
   graph/lane data first.
4. Render cross-view cable fragments with clear off-view continuation markers.
   Honor cable-target/peer behavior once the visual-settings model exists.
5. Add authored lane color and lane context actions, then surface the color
   consistently but modestly across views.

### P2 — Make the implementation architecture match the direction

6. Replace the large hand-written webview HTML scripts with React + TypeScript
   applications, separate `esbuild` entrypoints, and explicit typed host/webview
   message contracts. Migrate one surface at a time, starting with lanes.
7. Extract extension-host services (lane registry, focused-view/reveal service,
   transport/status coordination) behind injected interfaces; keep React
   components prop/hook driven rather than using a service locator.
8. Move high-frequency and selected-view actions to native VS Code menus,
   title actions, and keybindings. Add a real transport status indicator to the
   status bar; retain commands as the primary action surface.

### P3 — Saved working sessions

9. Implement named, ordered lane workspaces, last-active restoration,
    dropdown/cycling, and exclusive panel-set switching. Treat broader editor
    context restoration as a best-effort follow-up constrained by VS Code.

## Recommended first implementation slice

Take P0 items 2–3 together: add typed lane-view state and UI controls for the
lane query plus visual-settings query, initially with the current default
header anatomy encoded as settings. This establishes the enduring model needed
by navigation, cable policy, colors, workspaces, and the React migration while
avoiding new JSON-RPC routes prematurely.
