# VS Code UI Architecture Direction

This document captures the current intended direction for the Intravenous VS
Code UI before implementation begins.

It is meant to stabilize both:

- the UI product shape
- the VS Code-side implementation architecture

This should be treated as the UI-side counterpart to the runtime and project
architecture notes already present under `docs/`.

## High-level product split

The UI is made of three distinct but coordinated surfaces:

- a shared Intravenous toolbar surface in the VS Code activity container
- a live graph sidebar
- separate lane-view panels

These surfaces are all first-class.

The live graph sidebar and lane views do not replace each other.
They have different responsibilities and should both remain part of the
product.

## Surface responsibilities

### Shared toolbar

The shared toolbar owns:

- transport controls
- transport status
- lane workspace commands
- workspace switching controls
- reveal/focus commands when appropriate

Transport controls should live here primarily rather than being duplicated as
the main control surface inside every lane panel.

Lane panels still display the transport cursor.

Frequently used actions such as pause/resume should remain obvious visible
buttons.

### Live graph sidebar

The live graph sidebar is the graph/node/port inspection and editing surface.

It should expose:

- logical nodes and members
- all supported port states
- current authored and effective port state
- current/default values where relevant
- lane-backed state summaries
- reset-to-default actions
- reveal-to-lane actions

It should be possible to reach all existing runtime-supported input/output port
state transitions from this UI.

The live graph is not the only source of lane-related navigation, and it should
not own a special reveal-panel policy by itself.

Reset means sending the port back to `default` state.
This eventually removes the corresponding authored state entry in the owning
execution-side module.

Visually, the current live graph sidebar style already meshes well with VS
Code and should not be radically changed.

It should stay close to a VS Code-friendly inspector/tree tool rather than
becoming a mini canvas or a heavy property sheet.

### Lane-view panels

Lane views are separate VS Code panels and should behave like timeline track
canvases, not list snapshots.

Each lane view contains:

- a collapsible text query bar for lane selection/filtering
- a separate collapsible text field for visual-only behavior
- a horizontal timeline axis
- a vertical stack of lane tracks
- a transport cursor
- lane-kind-specific content widgets
- local cable fragments and off-view continuation indicators

Lane views are manually maintained project-authored views.

Visually, lane views should aim for:

- a studio-dense information layout
- a restrained professional-tool tone
- direct but precise interaction

Lane kinds should have moderate visual identity, while the overall workspace
should still read as one coherent system.

The lane workspace should carry the stronger bespoke visual language of the
product, while the live graph stays closer to the current VS Code-native tool
feel.

## Lane-view model

Each lane view has a stable UUID.

Each lane view owns at least:

- a lane query
- a visual-settings query

The lane query determines which lanes belong to the view.

The visual-settings query is a separate tiny language for visual-only behavior.

This visual-settings language should be extensible and should be the place for
any customizable visual lane-view behavior.

It must support at least:

- horizontal sync behavior
- cable-targeting / peer-view behavior
- lane-header content policy
- metadata/tag visibility policy
- noise suppression for identical metadata
- optional hiding of lane header metadata

The query field and the visual-settings field are separate UI elements and both
belong to the lane view.

The visual-settings language is not just about sync and cable targeting.
It should be the general place for view-specific presentation policy.

This includes things like:

- which lane fields appear in tiny headers
- whether the header emphasizes lane kind, user-defined lane name, or other
  lane details
- which differentiating tags/values should be displayed even when they are not
  part of the query
- stripping tags/values that are identical across all visible lanes to reduce
  noise
- hiding this metadata entirely for views that do not want it

There should be no single globally-correct lane header layout.
Different lane views may want different presentation rules.

## Lane color

Each lane should own an authored color as project state.

Lane color is not merely a per-view decoration.

The same lane color should be available consistently across views, although a
view may still choose how strongly to surface it visually.

Color use in the lane workspace should be more track-tinted than previously
assumed, while still remaining within an overall restrained professional-tool
visual language.

Lane color editing should happen primarily from lane views through context
actions.

## Cross-view cables

Cables do not belong to a single view.

Each lane view reads the connectivity state it cares about and displays the
visible local fragments/endpoints of those cables.

When only part of a cable is visible locally, the view should show off-view
continuation rather than pretending the cable ends there.

There is no need for one shared overlay spanning all VS Code panels.

## Transport behavior

Transport controls belong primarily to the shared toolbar.

Lane views display the current transport cursor position and react to shared
transport state.

The toolbar should cover:

- pause
- resume
- start-position-oriented transport actions

Transport controls should remain in the shared toolbar rather than becoming a
major always-visible control strip inside each lane panel.

## Live graph <-> lane view navigation

Navigation must be bidirectional.

It must be possible to navigate:

- from the live graph panel to a lane
- from a lane to the live graph panel

### Graph to lane reveal policy

Lane views are manually maintained by the user.

There does not need to be a special lane view or reveal panel policy for
mapping DSP graph logical nodes to lanes.

Instead, lane views should simply be easy to filter around the currently
selected DSP node in C++ editors, and those lane views remain user-managed.

The important navigation direction is the reverse one: from lanes back to DSP
source / DSP graph context.

This means source/editor selection should be able to drive lane filtering
cleanly, without introducing a dedicated auto-managed lane view just for
DSP-node-to-lane mapping.

### Lane to graph reveal policy

From a lane view, it should be possible to reveal the corresponding node/port
in the live graph sidebar.

That reveal should focus the live graph UI and surface the corresponding
node/member/port row.

This mapping is expected to be many-to-one in general.

The lane-to-source direction is the more important one to get right.

## JSON-RPC transport stance

The existing JSON-RPC surface should be treated as the intended transport
surface for supported UI actions.

Be very skeptical before introducing new JSON-RPC methods.

If something appears missing, first verify whether it is already achievable
through:

- existing lane-view open/update/close requests
- existing lane-view update notifications
- existing graph query/get methods
- existing graph mutation methods
- existing playback methods

If richer UI data is needed, prefer enriching existing payloads over inventing
new transport routes.

In particular, reveal/navigation should be implemented first as client-side
policy over the existing transport model.

For supported UI actions, the existing JSON-RPC surface should be assumed to be
the intended transport until proven otherwise.

If richer UI behavior needs richer data, prefer enriching existing payloads
instead of inventing extra routes casually.

## VS Code-side architecture

The VS Code client should be rebuilt as a typed application rather than
continuing with large hand-written DOM scripts embedded directly into webview
HTML strings.

The intended split is:

- extension-host orchestration layer
- React webview applications
- typed message contracts between them

### Extension-host responsibilities

The extension host should own:

- workspace session lifecycle
- JSON-RPC socket client ownership
- notification routing
- panel/view registration and lifecycle
- lane-view registry
- reveal/reuse policy
- VS Code-specific commands and focus logic

Webviews should not talk raw JSON-RPC directly.

### Webview responsibilities

Webviews should own:

- rendering
- local interaction state
- user intent dispatch back to the extension host
- consumption of typed snapshots/updates

The overall extension should remain hybrid:

- rich bespoke UI where the product genuinely needs it
- native VS Code integration where that improves workflow without bloating the
  product

Native VS Code integration should be used especially for:

- command palette flows
- context menus
- panel/view title actions where they clearly help
- lightweight toolbar/status feedback

The product should not try to turn every interaction into a custom webview
widget if VS Code already has an appropriate native surface for it.

## Technology choices

The intended client stack is:

- React
- TypeScript
- esbuild
- `tsyringe`

### React

React should be used for the live graph UI and lane-view UIs.

The UI is stateful enough that continuing with manual DOM mutation would make
the implementation harder to reason about and maintain.

### TypeScript

The whole VS Code client should move to TypeScript:

- extension-host code
- webview code
- shared protocol/model code

### esbuild

Use esbuild for bundling.

It should build separate entrypoints for:

- extension-host code
- live graph webview
- lane-view webview
- any additional shared UI surface that needs its own bundle

### tsyringe

Use `tsyringe` for dependency injection in the VS Code client to simplify
wiring.

This should primarily apply to extension-host-side services such as:

- workspace/session orchestration
- panel registries
- reveal/focus services
- transport coordination
- typed protocol adapters

Do not use dependency injection as an excuse for service-locator usage inside
React components.

React components should still receive explicit typed props or use explicit
hooks/stores.

## State management direction

Do not introduce a heavy general-purpose state library by default.

The intended approach is:

- custom typed stores
- custom typed hooks
- explicit extension-host owned authoritative session state
- webview-local presentation state where appropriate

This should be sufficient for:

- transport state
- lane-view registry state
- graph snapshot state
- lane-view structure state
- lane-view visualization state
- reveal/focus coordination

## Lane-view commands

Lane-view-local commands should mostly be exposed as:

- command palette actions
- keyboard shortcuts
- context menu entries

These commands should operate on the currently selected lane view when
appropriate.

That selection-sensitive behavior is important.
Lane-view commands should naturally target the currently selected/focused view
rather than requiring the user to route them manually every time.

They should not default to a large always-visible local command strip inside
every lane panel.

Very high-frequency actions belong in visible toolbar/buttons only when they
truly justify the constant screen cost.

## Workspace model

Lane-view layouts should support multiple saved named workspaces.

These workspaces are conceptually closer to desktops/workspaces in a window
manager than to loose ad hoc snapshots.

Each workspace should be:

- named
- ordered
- restorable
- quickly switchable

The workspace order must be customizable.

Fast switching should support:

- switching through a dropdown
- cycling through workspaces in listed order with shortcuts

On project open, the last active workspace should be restored by default.

Switching between workspaces should be exclusive and desktop-style:

- leave the current workspace
- hide/close its lane-panel set as needed
- restore the target workspace's lane-panel set/layout

If VS Code offers features that help approximate this desktop/workspace
experience, the implementation should try to rely on them.

## Workspace scope ambition

The intended UX ambition is broader than just lane-panel placement.

The design goal is closer to a full working-session restore around
Intravenous-owned UI state.

That means the design should aim to restore not only lane views but also a
broader VS Code working context where feasible.

This is a product goal even if implementation later has to approximate it due
to VS Code constraints.

The design should therefore state the full ambition clearly rather than only
describing the subset already known to be easy.

## Visual design work before implementation

The UI should be visually designed before implementation rather than discovered
on the fly.

Implementation should follow a dedicated visual design pass covering at least:

- overall visual language
- spacing and density
- typography
- color roles
- lane panel chrome
- live graph row anatomy
- badges/state indicators
- connection/cable rendering
- toolbar layout
- interaction states

The visual design should preserve what already works well about the current
source-editor integration and live graph panel while giving the lane workspace
its stronger visual identity.

The source-editor integration direction remains close to the latest
highlight-plus-panel behavior that was working well.
There is no strong desire to introduce heavy inline editor furniture beyond
that.

Source-editor integration should therefore stay focused on:

- highlight/reveal behavior
- lane filtering around the current DSP source selection
- efficient navigation from lanes back to source

It should not drift into a heavy CodeLens/hover-driven inline UX unless a later
need clearly justifies it.

This design work should produce a concrete UI design spec before serious UI
implementation begins.

## Current implementation implications

The current VS Code UI is behind the runtime model and should be treated as
outdated.

The runtime already has the important conceptual pieces for:

- long-lived lane views
- lane query updates
- lane-view notifications
- visualization notifications
- shared application-wide project actions

Therefore the main work is to make the client correctly reflect and exploit the
runtime model rather than inventing a separate UI-only architecture.
