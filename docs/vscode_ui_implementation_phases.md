# VS Code UI Implementation Phases

This note is the implementation-phase companion to
[`vscode_ui_architecture_direction.md`](./vscode_ui_architecture_direction.md).

It breaks the current UI direction into self-contained features, starting with
the parts that already exist partially or fully in the current VS Code client.

## Phase 1. Client foundation refresh

- switch the VS Code client to TypeScript
- add an esbuild-based build pipeline
- add the core client dependencies needed for the new foundation
- introduce `tsyringe` for extension-host-side dependency wiring
- preserve current user-visible behavior as closely as possible

## Phase 2. Session and notification core

- refactor workspace/server session ownership into typed services
- centralize JSON-RPC request dispatch and notification routing
- keep webviews from talking raw JSON-RPC directly
- preserve the currently used RPC routes as the baseline surface

## Phase 3. Live graph sidebar modernization

- rebuild the existing live graph sidebar on the new client foundation
- preserve its current VS Code-friendly inspector feel
- keep current working graph/source query behavior
- finish the sidebar so it exposes all currently supported port state families
- add full reset-to-default coverage

## Phase 4. Source integration completion

- preserve the current highlight + panel workflow that already works well
- make source/editor selection drive lane filtering cleanly
- support navigation from lanes back to source / DSP graph context
- avoid a heavy inline editor UX

To drive lane filtering from source selection, the runtime needs an app-module
path that can annotate lanes related to the currently selected logical node
with a suitable tag such as `port.selected` or a better-named equivalent.

The implementation should first look for an existing app module that can own
that responsibility cleanly before inventing a new one.

## Phase 5. Lane panel upgrade

- rebuild the current lane panel on the new client foundation
- preserve the fact that there is already a lane panel today
- move it from a snapshot/list-style implementation toward the lane-view model
- keep stable view identity and query-driven state

## Phase 6. Lane visualization integration

- consume the lane-content notifications already present in the runtime
- render lane content in the panel instead of only structural lane lists
- introduce the first pass of real track-canvas behavior
- surface authored per-lane color in the lane workspace

## Phase 7. Lane query and visual-settings editing

- make the lane query editable per view
- make the visual-settings language editable per view
- let visual settings control:
  - header policy
  - tag visibility
  - metadata noise suppression
  - optional metadata hiding
  - sync behavior
  - cable-targeting behavior

## Phase 8. Lane context actions

- add lane workspace context actions
- add color editing from lane views
- add reveal/focus actions from lanes
- add command-palette and shortcut coverage for selected-view actions

## Phase 9. Shared toolbar surface

- add the shared Intravenous toolbar surface
- expose frequent transport controls there
- expose workspace-level lane commands there
- keep lane-panel-local chrome compact

## Phase 10. Multiple lane views

- support multiple independently open lane views
- introduce a proper lane-view registry
- track selected/focused view state
- preserve the fact that lane views remain user-managed

## Phase 11. Cross-view connectivity rendering

- render local cable fragments inside each lane view
- show off-view continuation where appropriate
- keep connectivity rendering per-view rather than through one giant global
  overlay

## Phase 12. Named lane-view workspaces

- support multiple saved named workspaces for lane views
- make workspace order customizable
- support fast cycling through workspaces in listed order
- support switching through a dropdown
- restore the last active workspace on project open
- make switching exclusive and desktop-style

## Phase 13. Broader session restore

- extend the workspace model toward a broader VS Code working-session restore
- try to restore more than just lane panels where VS Code allows it
- keep the full-session restore ambition visible even if the practical
  implementation initially approximates it
