# Vue Client Implementation Stages

_Status: implementation coordination, accepted 2026-11-12. This document orders the
remaining Vue-client and observation work. It is not normative design: the semantic
contracts remain in
[Node Presentation And Manual Controls Direction](./node_presentation_and_manual_controls_direction.md),
[Node Interaction And Presentation Communication Direction](./node_interaction_and_presentation_communication_direction.md),
[Vue Output Observation And Query Direction](./vue_output_observation_direction.md),
and [Project Graph Application Architecture](./project_graph_application_architecture.md).
It replaces neither the [development workflow](./development_workflow.md) nor
[Outstanding Implementation Fronts](./implementation_fronts_direction.md); it is
deliberately not another fronts-style roadmap._

The remaining work is cut into six stages. A usable Vue graph editor comes before the
observation machinery. The Vue client is not the last integration step: it is the
environment in which the remaining backend capabilities are developed and validated,
so each backend API is designed against a real client requirement.

The stages are feature boundaries. They are not instructions to keep each
intermediate commit compiling or to build temporary compatibility layers. Stages 4
and 5 reuse the same observation-operation contract and the same `Observations`
application module.

## Why this order

On 2026-11-12 much of the difficult backend work already exists in source:
`graphConnections.upsert`/`graphConnections.delete`, `manualControls.*`,
`nodeMessages.dispatch`, `nodeMessages.getProtocol`, and `session.subscribe`. By
contrast, the source still contains the old presentation-registration machinery,
has no Vue dependency, and has no `Observations` module. The first goal is
therefore to expose existing functionality properly, not to expand the execution
system.

## Stage 1 — Vue extension foundation

A coherent replacement of the old webview's presentation infrastructure. The VS Code
extension stays; its webview renderer is replaced.

- Remove the `IV_NODE_PRESENTATION` pipeline (package finalizer, loader,
  `PackageRevision`, `IvPackageInfo::node_presentations`, socket RPC records), the
  VS Code `NodePresentationRegistry`, and obsolete frontend rendering assumptions,
  together with their tests. No adapter.
- Keep the VS Code extension host, workspace/session management, and socket
  connection infrastructure.
- Introduce Vue, a frontend-owned presentation registry keyed by stable leaf/module
  definition ID, the generic fallback, the four display modes, and the host-level
  semantic node context.
- The generic component needs only enough real graph data to display a node and its
  ports. No observation API.

**Completion criterion:** open an existing project in VS Code and inspect its nodes
in Vue, including after a session reconnect.

## Stage 2 — Functional graph editing

This stage deserves substantial attention.

- `VirtualNodeInfo` is a starting point. The Vue graph editor needs a coherent
  authoritative projection of node identities, ports, and connection information.
  Build it from the actual UI requirements, not from new generic query routes
  designed in advance.
- Cover creation and removal of node instances, connecting and disconnecting ports,
  configuration editing, and save/reload.
- Consume committed session updates correctly, including changes made by another
  client.
- Presentation geometry and layout stay in the frontend; graph semantics stay in the
  backend.

**Completion criterion:** build a small audio graph entirely through Vue, connect it
to an output, hear the result, save it, and restore it.

The first **undo/redo implementation slice** follows functioning instance and
connection editing and precedes relying on complex editing gestures: introduce
one batched reversible project mutation, an authoritative global session history,
and a multi-node delete/restore test. This is one coherent project-editing
feature, not a new general history framework in advance of Vue requirements.
Stage 3 then groups completed manual-control gestures in that same history.
The normative behavior is in
[Project Undo, Redo, And Backup Direction](./project_undo_redo_and_backups_direction.md).

After this stage Intravenous has a usable graph-building interface.

## Stage 3 — Node interactions and specialized presentations

- Generate TypeScript bindings from the existing semantic node-event protocol and
  expose the typed `node.send(event)` API.
- Connect the existing manual-control backend to Vue controls.
- Implement the interaction context that survives component remounts, mode changes,
  and graph revisions; this matters most for sliders, drags, and controls using
  dynamic Tick inputs.
- The specialized presentation registry initially uses only components bundled with
  the Vue extension. Third-party presentation distribution is not involved.

**Completion criterion:** a specialized component sends a typed event, operates a
manual control while audio runs, and stays coherent across presentation-mode changes
and backend graph revisions.

This is the first major interactive client milestone.

## Supporting cleanup — unify `Node::State`

Complete the agreed removal of authored `TockState` and the unification of the
lifecycle contexts (`InitializationContext<Node>`, `MoveContext<Node>`,
`ReleaseContext<Node>`), including Tock `move()` with its background-generation
ownership guarantees. Source on 2026-11-12 still contains `TockState`,
`TockInitializationContext`, and the related machinery.

This self-contained C++ API migration does not block stages 1–3; the initial graph
editor does not need it. It must be complete **before** stage 4, so that reflected
state observation is developed against the intended node API.

## Stage 4 — Live Tick observation, end to end

One complete feature, not separate standalone implementations of a registry,
scheduler, compiler instrumentation, and transport. Two real Vue requirements drive
it: one supported numeric Tick `State` field and a `Sample` output level meter.
They drive the source type registration, the `Observations` application module, the
reconciled-interest protocol, GraphJit probe installation, the executor handoff, and
the batched notification path.

Settled correctness requirements to exercise together:

| Requirement | What to verify |
| --- | --- |
| Structural graph edit with existing observations | Probes included in the same compilation |
| Control-code compilation | Stale control-code results cannot remove newer probes |
| Safe cutover | Values from obsolete compiled realizations cannot overwrite newer values |
| Source removal | Invalidation works even if the successor produces no frames |
| Multiple subscribers | Compatible interests share capture work |
| No subscribers | No unnecessary realtime probe work |
| Slow clients | Observation frames coalesce without damaging reliable session delivery |

Instrument observation-only compilation latency and realtime cost from the start.
Once the basic live pipeline works, add a bounded live waveform operation.

**Completion criterion:** rapidly pan, zoom, edit, and reconnect while meters run,
with correct subscriptions and no noticeable audio disruption.

## Stage 5 — Persisted and sparse Tock output observation

Reuses the type-registered operations and frontend interest machinery, over the
existing background-coverage and persisted-data infrastructure.

- Implement a real time-window query for a Tock output, with bounded projection
  before ephemeral transaction storage expires.
- Support a simple representative waveform first, then an accurate envelope when a
  user-facing requirement calls for it. Keep missing coverage explicit and preserve
  semantic/data-version consistency.
- Schedule the queries through the existing `BackgroundExecutor` without interfering
  with playback-critical materialization. This is the crucial integration work.

**Completion criterion:** view a long audio result, zoom from the whole song into a
short interval, and pan across it without retrieving or recomputing the whole
waveform unnecessarily.

Tock `State` observation remains unsupported.

## Stage 6 — Application-level verification

Every feature is tested while it is implemented; this final pass covers behavior that
spans features:

- a realistic mixed Tick/Tock graph;
- package refresh while the client is open;
- several simultaneous VS Code clients;
- an active control gesture during a graph revision;
- rapid viewport navigation;
- slow observation consumers; and
- persisted-data invalidation.

Repeat the existing latency benchmarks and compare against the pre-Vue baseline to
detect regressions caused by the observation infrastructure. This is not another
general Front 16 optimization effort.

## Deliberately postponed

- Tock `State` observation and input-value observation.
- Field-specific serialization overrides and arbitrary node-authored observation
  callbacks.
- Third-party Vue component distribution and hot reload.
- Advanced spectrum/event visualization catalogs.
- Multiresolution observation caches and speculative scheduling optimizations beyond
  what acceptable responsiveness needs.

None of these is a prerequisite for a complete initial graph editor or for exercising
the observation architecture.

## Milestones

| Milestone | Stages | Result |
| --- | --- | --- |
| A — Usable graph editor | 1–2 | Create, connect, configure, run, save, and reload graphs |
| B — Interactive node presentations | 3 | Typed events, controls, stable interactions, specialized components |
| C — Observable graph | 4–5 | Live meters, runtime state, persisted waveforms, and sparse Tock views |

Get the graph editor working before implementing observations, without waiting for
the whole Vue client to be finished.

## Current stage

Stage 3, started 2026-11-13. The owner has verified Stages 1–2, including
project history and durable settings history. Earlier checkpoints below retain
their original dates and are superseded by the later verification updates.

Current checkpoint (2026-11-14): generic lane wiring and live manual controls are
implemented and owner-tested. Complete the typed frontend `node.send(event)` API
and the specialized automation-curve presentation next. Its remaining scope
includes authoritative point data after reload/undo and grouped message gestures.
Live output meters and other Tick observation remain in Stage 4.

After Stage 3, revisit the display of unbounded knobs with the owner. The current
angle compresses values beyond approximately ±10, so widely different magnitudes
look almost identical. The owner requests useful visual magnitude feedback for
very high and very low values. The visual encoding is deliberately undecided.

### Stage 2 plan (2026-11-12)

The owner settled the editor surface: an editor is opened on one node, its
root, and opening a node as a root opens a new editor; node positions survive a
reload. See
[The graph editor is opened on one node](./node_presentation_and_manual_controls_direction.md#the-graph-editor-is-opened-on-one-node).
Stage 1 put the Vue application in the side panel instead, where it shows the
nodes of the active source file; the editor replaces it as the Vue host.

Order:

1. **Graph scope read model.** One read for one node address, the project root
   when absent: the scope's children with their node addresses, definitions,
   realization state, and ports (type, channel layout, direction); the
   connections among them, including unrealized instances and connections with
   a missing endpoint; and whether the scope is editable. `ProjectGraph` builds
   it under its lock from its current generation plus the desired state of
   `NodeInstances` and `GraphConnections`, so one read is one consistent
   revision. A committed change sends one notification that the graph changed,
   riding the existing `ProjectGraphPublication`; editors re-read their scopes.
   The project-root scope comes first; module-instance scopes, whose children
   come from the configured graph and are not editable, follow.
2. **Graph editor surface.** An editor tab per root hosts the Vue application.
   The root's presentation lays out its children with pan and zoom; each child's
   display mode follows its region. Positions are kept in a frontend-owned
   layout file in the project directory, keyed by node address. Without a saved
   position, children are placed in their authored order.
3. **Editing through existing routes:** `nodeInstances.create` from the package
   catalog, `nodeInstances.delete`, `graphConnections.upsert` /
   `graphConnections.delete` by dragging between ports,
   `nodeInstances.update` for configuration source, and `project.save`. A
   window reload restarts the server and replays the project. Hearing the
   result means connecting to an `iv.builtin.system_audio_output` instance.
4. **Retire `ModulesViewProvider`,** whose instance management the editor
   takes over.
5. **The first undo/redo slice** described above.

Not scheduled yet: project-authored groups, collapsible subgraphs with their
own inputs and outputs, defined in the project rather than in a C++ module
(see the editor section linked above). The scope read model treats a scope as
a node address with children so that a group can become one more scope.

### Stage 2 implementation status

Updated 2026-11-12. The owner reports that the canvas checkpoint passed tests
and manual UI checks. The replacement primary timeline and its new geometry
tests are written but have not been compiled or run by the coding agent.
The preceding status, including the canvas checks, is preserved in
[Vue Stage 2 Before The Primary Timeline](./historical/vue_stage_2_before_primary_timeline_2026-11-12.md).

Owner follow-up: the primary timeline passed tests. The subsequent reusable
lane changes described below still require compilation, tests, and UI checks.

- The default frame is now `NodeLane`. The enclosing presentation supplies one
  lane per child; `PresentationHost` still selects specialized components by
  definition ID. A specialized component may use `NodeLane` or its own layout.
- `TimeRuler` and the tick geometry live under `webview/timeline/`, independently
  of graph nodes. Port identities and metadata remain separate; the default
  lane uses grouped input/output counts in its left area, with full port metadata
  on hover. It does not nest the old panel's `PortRow` lists in its content area.
  No connection gestures are implemented by these metadata summaries.
- Lane height now comes from fixed initial vertical zoom, independent of the
  viewport size or visible child count. The current initial height is 240 CSS
  pixels, a tuning value. Overflow uses a vertical scrollbar. The bindings below
  change zoom explicitly. The superseded fit-to-height layout is preserved in
  [Lane Layout Before Fixed Vertical Zoom And Source Selection](./historical/vue_lane_layout_before_fixed_zoom_and_selection_2026-11-12.md).
- The initial display name is the user label when present, otherwise the last
  component of the definition ID. Full IDs remain intact for routing and debug
  details. Ordinary lane text no longer contains the instance UUID.
- A red name now requires the scope's classified `failed` state. Pending
  definition availability may still have diagnostic text but does not turn a
  lane red. See [Classified scope state](#classified-scope-state) below.
- The small left-side button copies the extension host's latest node snapshot:
  stable address, definition and configuration, realization state, diagnostic,
  ports, generation, and related connections. It uses the existing host session
  and VS Code clipboard API, without a new backend mutation route.
- The proposed expanding port area is recorded in
  [Node Presentation And Manual Controls Direction](./node_presentation_and_manual_controls_direction.md#proposed-port-area-and-temporary-expansion).
  It is not implemented in this pass. The next connection-editing work must
  retain individual endpoint identities and support arbitrary port counts.

- The accepted primary layout has time on the horizontal axis and one child node
  per row, ordered vertically. `TimelineScopePresentation` is the generic
  graph/subgraph presentation. The project root uses the same presentation host
  as its children.
- `GraphEditorProvider` still reads `graph.getScope` through the existing
  `WorkspaceSession`, refreshes on `graph.changed`, and reloads after a server
  restart. The scope contains desired nodes and connections; continuous ports
  include value-type metadata. No second server or graph owner was added.
- The timeline allocates unscaled child display regions. `PresentationHost`
  selects `compact` from width first; otherwise allocated height selects
  `nano`, `mini`, or `editable` unless a host supplies an explicit view choice.
  The height thresholds and initial row heights are provisional layout tuning.
- The initial horizontal scale is 0.01 seconds per CSS pixel, a tuning value.
  Resizing changes the visible time extent without changing that scale. The
  ruler describes display coordinates only, with no fabricated output data or
  playback position. Output observation remains absent.
- The ruler and lanes share a scrolling container so their display regions
  remain aligned when the node list needs a vertical scrollbar. Individual lane
  contents do not require a time grid; the earlier implementation is recorded in
  [Primary Timeline Before The Default Lane](./historical/vue_timeline_before_default_lane_2026-11-12.md).
- The owner's viewport bindings are implemented: native vertical scrolling;
  Alt+scroll vertical zoom; Shift+scroll horizontal scroll; Ctrl+scroll horizontal
  zoom; and held middle-button movement to pan both axes. Zoom anchors at the
  pointer where scroll bounds allow. Wheel line/page units are normalized.
  When modifiers are combined, Ctrl takes precedence over Alt, then Shift.
- Middle-button release, cancellation, lost capture, or window blur ends panning.
  A quick middle click neither changes the viewport nor enables a persistent
  movement mode. No clip-editing, node-reordering, or connection gestures were
  added.
- Horizontal position/scale, lane height, and vertical scroll position are saved
  in the existing frontend layout file under `timeline`. Navigation neither
  modifies project intent nor requests graph compilation. The earlier unbound
  viewport checkpoint is preserved in
  [Startup Status Before Classified Scope State](./historical/vue_startup_status_before_classified_scope_state_2026-11-12.md).
- The old `GraphScopePresentation` canvas source and its stored geometry are
  retained for a future alternative view; the primary timeline neither uses
  those coordinates nor generates new free-position node locations.
- Module-instance scope reads and opening a child as another editor root remain
  incomplete. Steps 3–5 also remain: graph mutation interactions, retirement of
  `ModulesViewProvider`, and the first undo/redo slice.

#### Interface availability after a failed build

The owner selected failure-only construction on 2026-11-13. Separate sessions
for every successful module build were also considered. The current import
path copies node bundles, remaps connections, and imports virtual-node records.
It would add work on cache misses, and constructor `NodeRef` arguments can
cross package boundaries. Successful builds therefore retain direct construction
in the root session. No rollback log or per-instance interface preparation is
added to that path.

Implemented in source, awaiting owner compilation and tests:

- `NodeInstancesProjectGraphRequest` enables diagnostic preparation only for
  committed rebuilds. Provisional node-message previews leave it disabled.
- After an instance batch fails, `NodeInstances` groups requests by constructor
  references. Ordinary audio/event connections do not join these groups.
  Failed nodes and their transitive dependents are excluded. Successful
  ancestors and independent instances can still supply interfaces.
- Each group uses a fresh builder session and the existing configuration cache.
  A newly failing constructor discards that session and removes at least one
  request before another attempt. Multiple failures in one reference group can
  repeat construction. Worst-case construction work within that failing group
  is quadratic in its size. This cost occurs only during failure handling.
- `NodeInstanceFailureInterfaces` pins definitions and configuration lifetimes
  for copied port metadata. `ProjectGraphConfigurationFailure` owns that result.
  It is never submitted to GraphJit or GraphExecutor. Scope reads run no
  constructors and do not substitute ports from an older configuration.
- A later connection failure can use already configured instance results for
  current interfaces. An invalidated session cannot supply a non-reusable
  result, so that interface stays unavailable instead of showing stale ports.
- A successful rebuild clears the failure result through the existing path.
  The previous executable generation remains current throughout failed builds.

Light regressions cover initial interface availability despite a missing
definition, stale-port exclusion, error recovery, constructor-reference chains,
failed-constructor dependents, and disabled preparation for successful builds
or provisional callers. The original defect and alternatives are preserved in
[Scope Interface Availability Before Failure-Only Construction](./historical/scope_interface_availability_before_failure_only_2026-11-13.md).

#### Connection-editing implementation checkpoint

The owner requested these refinements on 2026-11-13. Implementation is written
but awaits compilation, tests, and UI verification by the owner. The earlier
button/native-drag checkpoint is preserved in
[Vue Port Actions Before Pointer Capture](./historical/vue_port_actions_before_pointer_capture_2026-11-13.md).

The root toolbar and lane edit button are removed. The extension contributes
commands for Add Node, Edit Node, Delete Node, Open Node Source, New Package,
and Delete Connection. Save remains the existing Save Project Now command.
These commands are available through the Command Palette and can receive user
keybindings. Node and connection commands use a picker for their target.
The copy-debug button requested earlier remains. Configuration/package forms
retain their completion controls. No default editing shortcut is invented.

Input handles are circles using `--vscode-terminal-ansiYellow`; output handles
use `--vscode-terminal-ansiBlue`, matching the source-selection side panel.
Each side uses a direct handle for one port and a grouped slot for more than
one. Input and output slots remain separate. Grouped ports use overlapping
circles, without a visible arrow/count label. Hover extends a panel directly from that slot. The panel
keeps a bounded scrollable list and does not change lane height. During a
connection drag, `NodePorts.hoverGroup` requires the hovered direction to match
the destination direction and `groupAccepts` to find at least one candidate.
It never expands a different side merely because the pointer passed over an
unavailable group. Direct handles stay exposed. Slot focus/Enter uses the same
checks while a drag is active.

The native HTML drag-and-drop path is replaced by a pointer gesture owned by
`GraphEditor`. It captures the pointer on the editor surface and hit-tests
the actual element under the pointer. Release on a compatible handle submits
`graphConnections.upsert` through the existing revision-checked RPC path.
A click, release outside a handle, incompatible port kind/direction, Escape,
lost capture, pointer cancellation, or window blur submits no connection.
Middle-button navigation is unchanged.

`NodePorts` closes its popover when `connectionDrag.port` changes at accepted
pointer-down, and closes destination popovers when the gesture ends. This
applies to both ordinary port rows and connection-chooser rows. Capturing the
pointer in `GraphEditor` keeps the gesture active after the row is removed.
`GraphEditor.sourceGroupToLeave` stores the source node key and input/output
direction. It prevents that group from being exposed again until the pointer
leaves it; later re-entry is allowed for moving an end to another port in the
same group. During a drag, `NodePorts.hoverGroup` accepts the editor's current
hit-test result rather than reopening from a layout-generated mouse-enter.
Source popovers therefore expose covered destinations before mouse release.

Applied connections fill their port circles. Unapplied desired connections
use dashed circles. Port tooltips identify their connections and application
state. The ordinary port list contains only its port rows, without connection
summaries or a grouped-icon cap. Deletion is an extension command.
Type conversion remains a backend decision.

Lane selection and connection display were requested on 2026-11-13 and are
implemented in source. Primary-clicking a lane's non-control area selects it.
Enter or Space selects a keyboard-focused lane. Port gestures and copy-debug
controls do not change selection. Selection is local to each editor and survives
scope revisions while its stable node address remains present.

`selectedLaneConnections()` projects incoming and outgoing connections into
visible endpoint pairs. A collapsed slot receives one wire per visible pair.
Opening that slot uses the exposed port-row anchors, so the wire separates into
the connected branches. Closing it merges the drawing again. Distinct visible
selected ports retain distinct wires. This never changes graph connections.

The SVG overlay is pointer-transparent. It measures anchors after scope, size,
selection, zoom-height, scroll, or port-panel changes. It uses animation frames
only to combine pending measurements, not as a continuous rendering loop.
Clipped rows in a scrollable port panel fall back to the slot anchor. Unresolved
interior selectors and absent interfaces do not acquire guessed endpoints.
Collapsed fan-in/fan-out groups ports by visible anchors before pairing them,
so collapsed groups do not enumerate all hidden source/target combinations.

Grouped input and output icons show empty, partly connected, and fully
connected states. The right circle fills when any port has a desired connection;
both fill when every port has one. Pending desired connections retain dashed
outlines. Opening the list replaces the grouped icon with its first port row.

`NodePorts.expand` and `alignFirstRow` position both ordinary port lists and
connection choosers. The first row's circle aligns with the opening control;
upward-growing lists keep their first row at the bottom. A version check
rejects queued alignment after the list closes or is replaced. The removed
cap and summary checkpoint is preserved in
[Vue Port Lists Before First-Item Alignment](./historical/vue_port_lists_before_first_item_alignment_2026-11-13.md).

The owner reported a first-open connection chooser appearing at the screen
origin when Shift-hovering a grouped slot whose first port was connected.
The prior code accepted detached or zero-sized row rectangles as popup origins
and allowed nested hover during positioning. `NodePorts` now rejects those
rectangles and keeps a panel hidden and noninteractive until its first-row
alignment is committed. It rechecks the version after reading geometry as
well. This prevents a nested chooser from using an unpositioned port row.
Confirmation of the reported first-open sequence remains a manual UI check.

`GraphEditor.connectionStartAnchor` holds the port-circle position for a new
connection preview. It refreshes with the measured port or its grouped-slot
fallback after panel and viewport changes. `PortConnectionDrag.start` retains
the original press coordinates for the movement threshold. Clicking elsewhere
inside the row therefore does not move the preview's starting point or make a
stationary click count as a drag.

Connection previews are SVG paths with per-wire gradients. `connectionWirePath`
generates both their curves and the finished `selectedLaneWirePath` curves.
`GraphEditor.previewWires` keeps each fixed anchor's input/output direction,
including every fixed end when reattaching a multi-ended connection. Its
gradient starts with that direction's theme color and ends with a CSS blend
of the same input-yellow and output-blue theme colors at the cursor.

`connectionCursorOutputFraction` uses only the cursor position and the closest
measured row's input/output column centers. It has no hovered-direction input.
`GraphEditor` caches those columns when geometry changes; pointer movement
interpolates cached coordinates. Hovering an input or output popover does not
force an endpoint colour. The gradient still starts with the fixed port's
actual theme colour. Colour does not select control-point direction or routing.
Light tests cover interpolation bounds, different row layouts, reversal of
curve endpoints, and the shared rule for self-connections and aligned ports.
Build, test execution, and manual colour checks remain for the owner.

`NodePorts` now marks unavailable destination-side handles, port rows, and grouped icons with
`.unavailable`, replacing the previous target rings. Its local `--port-color`
is a 25% normal-colour / 75% `--vscode-disabledForeground` mix; filled circles
and dashed outlines preserve connection state. Unavailable row text is also
muted. Ports on the other side retain their normal colour. Destination groups
remain active-looking when at least one underlying port can receive the drag.
The appearance does not disable hit-testing or change
connection validation.

For a new connection, `acceptsPortConnection` still checks opposite directions
and equal sample/event kinds; it does not establish that a converter exists.
For moving an existing end, `canMoveConnectionEnd` still applies its additional
record/type rules. The previous ring and opposite-side expansion are retained
in [Vue Port Targets Before Muted Unavailable Ports](./historical/vue_port_targets_before_muted_unavailable_ports_2026-11-13.md).

`GraphEditor.startConnection` captures source geometry, focuses the editor,
and takes pointer capture before assigning `connectionDrag`. The owner's
recording now identifies the first-press failure: the press reached the port,
started the gesture and acquired capture; a webview blur cancelled it about
1 ms later. The primary button remained held and webview focus returned about
50 ms later. The preceding status and relevant trace times are preserved in
[Vue Port Grab Before Native Window Focus Reporting](./historical/vue_port_grab_before_native_window_focus_2026-11-13.md).

`GraphEditorProvider` now forwards VS Code's `onDidChangeWindowState` as
`graphWindowFocus`, including the current state when a panel becomes ready.
`listenPortConnectionFocus` connects document blur and host messages to separate
callbacks. Document blur resets hover modifier state without cancelling the
captured gesture. Native window focus loss cancels it. Pointer cancellation,
lost capture, lost primary button, Escape, and scope replacement still cancel.
The listener and host subscription are removed with their owners.

`portConnectionFocus.test.ts` replays the owner's focus/down/blur/held-move/
focus/release event order through the production focus listener and gesture
functions. It checks that this sequence can complete a connection and that a
native-window blur still cancels without revival on focus gain. It does not
reproduce the operating system's activation itself. Build, test execution, and
manual verification remain with the owner.

The owner confirmed that native-window activation and the connection previews
worked. The temporary recording command, protocol messages, instrumentation,
recorder, and recorder-only tests have been removed at the owner's request.
The focus fix and its event-order regression tests remain. The removed diagnostic
and its manual check are preserved in
[Removed Port Grab Recorder](./historical/vue_port_grab_recorder_removed_2026-11-13.md).

`GraphEditor.dropPreviewWires` adds a faint, attached preview for the individual
eligible port under the pointer while preserving the free cursor cable. The
destination popover stays open. Anchor measurement rejects hidden, clipped,
and still-positioning rows; layout, scroll, and popover changes refresh the
preview as well as pointer motion. `canDropPortConnection` supplies the shared
hover and release checks, including reattachment's record/type rules and
rejection of its original port. Eligibility tests cover both drag directions,
kind/direction mismatches, read-only scopes, no-op reattachment, stale records,
and shared type constraints. These checks do not prove converter availability.

`connectionWirePath` uses one cubic with both control points at the horizontal
midpoint between endpoints and with their respective endpoint Y coordinates.
It accepts only the two points. `selectedLaneWirePath` and drag previews use
that same function. `SelectedLaneWire` no longer contains self-connection
routing fields, and wire planning no longer assigns separate return paths.

`GraphEditor.measureWireAnchor` measures the right edge of an input circle or
the left edge of an output circle at its vertical midpoint. Established wires,
connection entries, pickup anchors, and faint destination previews share this
measurement. Collapsed slots use the same function with the outer circle on
their cable side. The cursor end of a free cable continues to follow the mouse.

The previous routing is retained in
[Vue Wires Before The Single Cubic Rule](./historical/vue_wires_before_single_cubic_rule_2026-11-13.md).

Selection-display regressions cover both directions, unrelated edges, grouped
slots and expansion, distinct visible ports, wildcard fan-in/fan-out, missing
anchors, pending state, event ports, and collapsed-group work. Compilation,
tests, and UI verification remain with the owner. The preceding display rule
is preserved in
[Vue Connection Display Before Lane Selection](./historical/vue_connection_display_before_lane_selection_2026-11-13.md).

Instance/configuration editing, package creation, source navigation, and save
use the Vue forms and existing routes. The host rereads scopes before mutations.
Configuration updates compare the form's starting values with the current
accepted record before submitting through the revision-checked RPC path.
`ModulesViewProvider` and its old command/serializer remain removed. The
source-selection side panel remains, including its filtering.

Client regressions cover endpoint identity, channel/event conversion metadata,
read-only/stale endpoints, pointer ownership, drag versus click, and invalid
or cancelled releases. No tests or builds were run by the coding agent.
The first server-owned undo/redo slice is now in source; see the implementation
checkpoint below for its scope and pending verification. Stage 3 has not started.
The preceding status is preserved in
[Historical Undo/Redo Direction](./historical/project_undo_redo_before_first_implementation_2026-11-13.md).

Shift-drag disconnection and endpoint movement are implemented in source.
Shift-hover opens a chooser for any port with at least one connection. Pressing
Shift while already over the port works too. `NodePorts` measures the hovered
circle, then aligns the first connection's circle with it after rendering.
The connection chooser has no decorative cap. If it grows upward, its first
entry is at the bottom, under the pointer. Connection labels stay on one line
with their full text and port information in the tooltip. Queued positioning
is discarded if the chooser closes or another chooser replaces it.
Direct Shift-drag still picks up a sole connection; Shift-click remains a
fallback. Right-click receives no new menu.
The connection chooser requires held Shift. Key release hides it; for a grouped
port slot, the ordinary port list remains available. Releasing Shift after an
entry has been picked up does not cancel its captured drag.

`GraphEditor.hoverShift` is the shared Shift state for port interaction.
Keyboard events and explicit pointer-button events update it. Pointer movement
updates it when the pointer ID or client coordinates change. Replacing rows
under a stationary pointer does not count as a modifier change: enter/leave
events and unchanged-position movement do not overwrite the state.
`NodePorts` reads `GraphEditorContext.shiftHeld` and watches it to open or close
the connection chooser. It no longer infers a Shift release from a separate
local pointer modifier handler. Its leave handler also checks the current element under the
pointer when a boundary event refers to a row that has been replaced.

The owner then reported port popovers remaining open after slow right-side
or bottom-side exits. The previous leave path could return during positioning
or accept a boundary hit without receiving another leave event. It also
counted the full `.node-ports` area, including the gap between groups, as owned
hover space. `NodePorts.checkPopoverPointer` now checks the opening
`.port-group` and the popover itself. Temporary window pointer-move and
pointer-out listeners are attached only while a popover is open and removed
on close or unmount. Movement records the latest pointer position even during
positioning; a post-layout check consumes that position once the panel is
visible. Boundary events request the same check without replacing the recorded
movement coordinates. Captured connection drags retain the editor's hover
handling. Confirmation of slow edge exits remains a manual UI check.

This addresses the reported held-Shift, first-entry flicker path. Light tests
cover row-replacement event sequences, keyboard press/release, real movement,
button events, and reset after blur. The unfocused webview sequence still
requires manual confirmation. Keyboard events reach the focused webview;
with focus elsewhere, a stationary key change remains unavailable until actual
pointer movement or a button event reports it. Hovering does not take focus.
The previous per-event handling is retained in
[Vue Shift Before Shared Modifier State](./historical/vue_shift_before_shared_modifier_state_2026-11-13.md).

Chooser entries expose connection-specific wire anchors through
`data-connection-end`. `selectedLaneConnections` uses both the connection ID
and port identity to attach wires to visible entry circles. Opening or closing
the chooser splits or combines those attachments without changing the records.
Chooser connections are visible even without a selected lane. A multi-ended
record exposes only the pairs involving the chosen port or the selected lane.
The graph changes only on release. Empty editor space removes the chosen end;
a compatible port replaces it. Invalid drops and cancellation leave the graph
unchanged. The preview stays anchored at the opposite end, including grouped
port slots and multiple opposite endpoints.

`WorkspaceSession.editGraphConnectionEnd` reads the current scope before
submission and captures the session revision before that read. A notification
between the read and submission cannot make an old edit pass the revision
check. `connectionEndMutation` rejects stale records, preserves other
endpoints and channel selections, and returns either one upsert with the same
ID or a deletion when one side becomes empty. It never submits delete followed
by upsert. Undo remains unimplemented; each completed gesture is intended to
be one future undo entry.

The agreed rules are in
[Disconnection And Endpoint Movement](./node_presentation_and_manual_controls_direction.md#disconnection-and-endpoint-movement).
The earlier proposal and checkpoint are retained in
[Connection Gestures Before The Shift-Only Chooser](./historical/vue_connection_gestures_before_shift_only_chooser_2026-11-13.md).
Light TypeScript tests cover release decisions, both moved directions, multiple
connection choices, stale records, channel selections, wildcard port selectors,
and preservation of other endpoints. Build, test execution, and manual pointer
checks remain for the owner.

#### Classified scope state

Implemented in source on 2026-11-12; compilation and tests remain for the owner.
The earlier investigation is preserved in
[Startup Status Before Classified Scope State](./historical/vue_startup_status_before_classified_scope_state_2026-11-12.md).

`graph.getScope` now returns a typed `loadState` for each node:

| State | Meaning |
| --- | --- |
| `waitingForDefinition` | The definition is unavailable and no completed package result confirms failure. |
| `waitingForGraph` | The definition is available, but the instance has no successful realization. |
| `ready` | The instance is realized, with no reported configuration or package-build failure. |
| `failed` | A package build failed, configuration failed with the definition available, or a completed package did not publish the requested definition. |

`ProjectGraph` reads `PackageDefinitionsProjectGraphRead` once in the scope
read tree, alongside desired instances and connections. The package owner
copies its catalog and `publication_pending` under one lock. Its
`completed_publication_generation_` advances only after synchronous
definitions/project publication returns. A read between installing a build
result and publishing its definitions therefore keeps an unavailable definition
pending instead of displaying a transient failure.

The lane uses `loadState == failed` for red styling, not the presence of an
availability diagnostic in `errorMessage`. Diagnostic text remains available
for copied debug details. Failure can coexist with a previously realized
generation that is still running.

The extension also refreshes the scope on the existing `ivPackages.updated`
notification. A failed package build need not create a graph generation or emit
`graph.changed`. No new public RPC route or diagnostic-string matching was added.

#### Ruler labels and playback-position investigation

The owner reported 882 of 883 C++ tests passing after the viewport changes.
The remaining scope-recovery test threw before reading the scope: its create
request omitted `package_root` while the definition was still unavailable.
The fixture now supplies that path, as the other pending-definition test does.
This correction awaits the owner's next test run.

Ruler labels now remove trailing decimal zeros after rounding floating-point
multiplication noise. A shared mark stays `1 s` across tick-spacing changes.
A client regression covers shared labels across zoom levels, fractional labels,
negative positions, and zero. This also awaits the owner's test run.

**Observed implementation:** `SystemAudioDevices::output_render_loop()` owns
`realtime_cursor_`, advances it for each rendered block, and passes it to
`GraphExecutor::tick_block(sample_index, block_size)`. The cursor advances even
when there is no active graph. The current `SocketRpcRouteRegistry` and request
types expose no playback-position read, play/pause, or seek operation. The
existing `server.status` notification reports build/status messages, not position.
The frontend ruler therefore still describes viewport coordinates only.

**Existing design:** [Global Indexing](./global_indexing.md#sequential-execution-order)
requires Tick execution to continue while playback is paused; resume or seek
can select a different global coordinate. Stopping audio rendering to implement
pause would contradict that requirement. The current device callback does not
yet implement those playback controls.

The owner agreed to settle the playback contract on 2026-11-13. Its destination
is now recorded in
[Project Playback And Presentation Grids](./project_playback_and_presentation_grids_direction.md).
The core contract separates the selected project position from continued Tick
execution, uses samples and seconds for the same position, and assigns applied
playback state to `GraphExecutor`. The owner selected a builtin callback
structure for nodes that need playback state. Ordinary Tock/persisted clip
sources do not need it: the existing contract supplies neutral/empty Tick
input views while paused. The initial playhead does not depend on adding a
node-facing accessor. No implementation or mouse/keyboard playback gesture is
claimed by this document change.

The same document records the owner's proposed grid source: a node output
projected into timeline marks through output observation. Grid selection does
not require new root graph ports or a project-wide tempo map. It follows a
concrete observation/editing feature and does not block the playhead.

After the playback contract is resolved, resume Stage 2's remaining scope/port
and editing work. The previous discussion text is preserved in
[Playback Discussion Before The Initial Contract](./historical/playback_design_discussion_before_contract_2026-11-13.md).

#### Shared lane UI in the source-selection panel

The owner wants the source-selection side panel to become a filtered host of
the same lane UI, with shared interaction/selection state across webviews.
Filtering is required and must not be deprecated. Before this replacement,
adapt the current `VirtualNodeInfo` source query to the same stable node/port
identity contract that the editor lanes use; do not carry over one display lane
per port or lose source-selection focus. The replacement is not implemented in
this pass.

#### Verify the primary timeline

After rebuilding the server and extension:

1. Run `Intravenous: Open Graph Editor`. Check that child nodes occupy separate
   rows under one horizontal time ruler.
2. Resize the editor vertically. Lane heights must stay fixed, with a vertical
   scrollbar when they do not fit. Width changes can still select `compact`.
3. Check a node list long enough to scroll vertically. The ruler and lane display
   regions must remain aligned. Check that the left-side copy button copies the
   full node identity and diagnostics, while the lane itself has no UUID text.
4. Reload the VS Code window and trigger a package refresh. The editor must
   return and refresh its nodes.
5. Check that there are no permanent interaction hints, spatial zoom percentage,
   fabricated output graphics, or unassigned editing gestures.
6. Exercise each viewport binding, then reload the window after navigation is
   saved. Middle-button release must stop movement immediately; a quick click
   must never start browser autoscroll or a persistent movement mode.
7. Start with uncached packages, then introduce a real build failure and recover.
   Waiting definitions must stay neutral. Confirmed failures must turn red and
   clear after recovery, including when an older graph remains active.
8. Use the Command Palette for Add Node, Edit Node, Delete Node, New Package,
   Open Node Source, Delete Connection, and Save Project Now. The editor has
   no permanent action toolbar or lane edit button. The copy-debug button remains.
9. Check yellow input circles and blue output circles against the side panel.
   Hover a grouped slot. Its panel must extend from the slot without an arrow/count
   label or a change in lane height.
10. Drag from an individual port to a compatible port, in both directions.
    A grouped destination must expose the opposite direction. Direct targets
    must remain available under the pointer. After release, check the filled
    applied-state circle and connection tooltip. An unapplied desired connection
    has a dashed circle. Select a lane to inspect its incoming and outgoing wires.
11. Cancel a drag with Escape, release outside a port, or switch away from the
    webview. These actions must submit no connection. A quick primary click
    must also submit nothing. Middle-button scrolling must still work.
12. Select a lane with incoming and outgoing connections. Hover a connected
    grouped slot in another lane. A wire to that slot must separate into wires
    to the exposed connected rows, then combine again when it closes. The
    original lane stays selected. Scroll or zoom vertically and check that the
    endpoints follow their controls. Port dragging and copy-debug clicks must
    retain their original actions.
13. Reload the webview. Hold Shift and hover a grouped slot whose first port
    has a connection. Its port list and connection chooser must stay at the
    opening control on the first attempt, without appearing at the screen
    origin. Close and reopen both lists; repeat with several connections.
14. Press near the text of a port row and drag to create a connection. The
    preview must start at the port circle, not the press coordinates. A
    stationary click must still submit nothing. If the source list closes,
    the preview must follow the source's grouped slot.
15. Focus another editor, then hold Shift before entering a grouped port whose
    first port is connected. The chooser must remain stable through the port
    list and connection-list transitions. Repeat without releasing Shift.
    Release Shift and move slightly to report the release to the unfocused
    webview. With keyboard focus in the webview, release must hide the chooser
    immediately, without reopening from a stationary boundary event.
16. Start a new connection from a port list that overlaps another lane. The
    list must close on press, and the uncovered destination must be usable
    before release. Repeat from a connection chooser. The source group must
    stay closed while the pointer remains on it; leave and re-enter it while
    moving an input end, then choose a different input in that same group.
    Destination lists must still open on hover. A click without movement and
    Escape must make no connection change.
17. Without clicking or using the keyboard, open a port popover and leave
    slowly through its right and bottom borders. Repeat at higher speed,
    across the gap between input and output groups, and during its first
    opening. It must close outside its own group and panel, stay open over
    either, and close when the pointer leaves the webview. Repeat with a
    connection popover while holding Shift; nested transitions must not flicker.
18. Start a new connection at an input. Its fixed end must stay yellow and
    the preview must use the same simple curve as a finished wire. Move across
    the input/output columns, including through an expanded popover: the
    cursor colour must follow its position without switching merely because
    it crossed a popover boundary. Repeat from an output and while moving an
    existing connection end. Self-connections must use the same cubic rule;
    aligned endpoints may be straight. Theme changes must update the gradient.
19. Focus another editor or window, then press a port once and drag. That
    first press must start the gesture and retain pointer capture while the
    source popover closes. Repeat from an individual handle, a port row, and
    a connection row. Genuine focus loss, Escape, and lost pointer capture
    must still cancel the gesture.
20. Drag a new connection from an output and pass over other output groups.
    They must retain their normal colour and must not open input popovers. Hover a compatible
    input group; it must retain its colour and open its own inputs. Rows of a
    different sample/event kind must be muted. Repeat from an input toward
    outputs and while moving an existing end. Cancel the drag and check that
    normal colours return, with filled and dashed states preserved.
21. Drag over an eligible individual port, including one exposed in a destination
    popover. A faint cable must show the completed connection at its circle
    while the original cable follows the pointer. The popover must stay open.
    Move between rows and away from the list: only the current eligible row
    receives this preview. Check both drag directions, self-connections, and
    reattachment. Incompatible targets must not receive a preview. Release or
    cancel: both temporary cables must disappear, with no edit on cancellation.
22. Check cable endpoints on inputs and outputs, including collapsed groups,
    exposed port rows, and connection-chooser entries. Each cable must meet
    the right edge of an input circle or the left edge of an output circle
    without crossing its interior. Repeat for the fixed end while dragging
    and both ends of the faint destination preview.

### Stage 1 implementation status (2026-11-12; built, tests pass)

Removed from source:

- `src/intravenous/node/presentation.h` (`IV_NODE_PRESENTATION`,
  `PackageNodePresentation`, `NodePresentationDescriptor`) and its `dsl.h` include;
- the finalizer's presentation-record collection, validation, and
  `iv_package_node_presentations` table (`tools/iv_package_finalize.cpp`);
- `iv_package_node_presentations_fn`, the loader's table copy and validation,
  `ModuleLoader::LoadedPackage::node_presentations`,
  `PackageRevision::node_presentations`, `IvPackageInfo::node_presentations`, and
  the `nodePresentations` field of `ivPackages.list` packages;
- the VS Code `NodePresentationRegistry` (`vscode/client/src/nodePresentation.ts`)
  and the hand-written DOM renderer inside `LiveGraphViewProvider`;
- the presentation parts of the affected C++ and TypeScript tests.

Added:

- `vscode/client/webview/`: a Vue application built by Vite
  (`vite.webview.config.mts`, invoked from `esbuild.mjs`) into `dist/webview`.
  `LiveGraphViewProvider` writes the CSP/nonce HTML and loads that bundle.
- Host/webview protocol (`src/liveGraphProtocol.ts`): the webview sends `ready`,
  `selectInstance`, and `createInstance`; the host answers every change with the
  complete `LiveGraphViewState`, including a `sessionStatus` of `starting`,
  `running`, or `restarting`.
- `webview/presentation/`: the frontend-owned `PresentationRegistry` keyed by
  definition ID, the generic presentation (`GenericPresentation.vue`), the four
  display modes with host-imposed `compact` (`displayMode.ts`), the
  `PresentationHost` that selects a presentation and provides the node, and
  `useNode()`.
- The window owns its server (`--rpc-fd`; see
  [Project Session Server Direction](./project_session_server_direction.md)):
  reloading the window restarts the server, and a server that stops while the
  window is open is restarted, after which the panel re-runs its source-span query.

Decisions and assumptions made to proceed:

- Toolchain: Vue single-file components built by Vite with `@vitejs/plugin-vue`,
  typechecked by `vue-tsc`; the extension host stays an esbuild bundle. A later
  standalone host can reuse the same Vite application.
- The node data is the existing source-introspection `VirtualNodeInfo`
  (`graph.queryBySpans` and `graph.nodesUpdated`). It carries no `NodeAddress`, so
  the stage 1 `SemanticNode` has no `address`; stage 2's graph-editor projection
  must supply it. The node `id` is the source-introspection virtual-node ID.
- User view choice (`nano`, `mini`, `editable`) is webview-local state; the host
  imposes `compact` below a provisional width threshold with hysteresis.
- No preference storage yet: with no specialized presentation registered, a stored
  preference would have nothing to choose between. It arrives with stage 3's
  specialized components, as a workspace preference.
- The separate "Intravenous: Open Modules" webview panel (`ModulesViewProvider`)
  still uses its hand-written renderer. It manages instance creation, renaming,
  and deletion, which stage 2's Vue graph editor takes over; it is replaced there
  rather than ported twice.

Corrected along the way: the client read `packageIdentity` from virtual nodes, but
the backend sends `sourceIdentity`, so the old panel's identity summary always fell
back to the raw node ID.

Resolved 2026-11-12: the client is installed and built, all tests pass, and the
owner runs it against existing projects, including window reloads and server
restarts.

### Undo/redo implementation in progress (2026-11-13)

The first Stage 2 history feature is now being implemented in this order:

1. Capture accepted before/after records inside the existing owner transactions;
   retain only changed records in history.
2. Validate a complete batch in the existing module-local transactions before
   committing it through one normal ProjectGraph rebuild.
3. Keep one session-global history in ProjectGraph, with record applicability
   checks, no-op handling, redo invalidation, and no history during project replay.
4. Expose revision-checked batch/undo/redo requests and extension commands, scoped
   to the graph editor rather than ordinary source-editor Ctrl+Z.
5. Add light tests for multi-node restore, retained dangling connections, changed
   replacements/controls, failed or stale actions, semantic replacements, and
   one publication/event-tree traversal per accepted action.

Canonical records stay with NodeInstances, GraphConnections, and ProjectGraph's
manual-control records. Configuration failure still accepts durable intent and
keeps the previous executable generation; history follows durable acceptance.
Build/test execution remains with the owner. This paragraph records work in
progress, not completion of the feature.

### First undo/redo implementation checkpoint (2026-11-13; not yet built/tested)

The implementation above is now present in source. `ProjectGraph` retains
`ProjectEditEffect` records and a history position. Owner transactions capture
accepted before/after snapshots; history retains only changed durable records.
`NodeInstancesInteraction` and `GraphConnectionsInteraction` now support reading
and validating a complete proposed batch before their single final commit.
No extra read event re-enters either owner in the action's event tree.

`project.applyEdit`, `project.undo`, and `project.redo` use the existing
permission/revision checks and publication path. The extension's Delete Node
command permits multiple selections and submits one batch. Undo/redo commands
and graph-local keyboard shortcuts use the same server-owned history for all
clients. Text controls keep their own shortcuts. Failing or stale validation
keeps records and history position unchanged; no-op edits preserve redo.
Configuration/finalization failure after durable acceptance is an undoable edit
while the previous executable generation stays active. Normalized replay resets
history, and the saved project format is unchanged.

The backend manual-control interaction path also coalesces consecutive updates
while preserving order across intervening accepted actions. Undo/redo is refused
until active manual-control interactions finish; client disconnect finalizes
the last accepted value. Vue manual controls and their longer-lived interaction
context still belong to Stage 3. Stage 3 itself has not started.

Added light regressions cover multi-node deletion/restoration (including owned
replacements and controls and retained dangling matchers), generation/publication
counts, failed validation, existing connection upserts, no-op/redo branching,
accepted configuration failure, replay reset, semantic replacement history,
manual gesture grouping/interleaving, and owner-record applicability checks.
Client tests cover revision-checked batch/history requests and text-safe shortcut
selection. The owner runs builds and tests; no pass is claimed here.

Manual checks after rebuilding/installing:

1. Delete two nodes through one Delete Node action. Undo once restores both;
   redo once removes both. Existing dangling connection records must survive.
2. Create, configure, connect, move an endpoint, and disconnect. Undo/redo each
   action once. A connection-end move must remain one step.
3. Undo, then make a different accepted edit. Redo must report no available step.
4. Edit from two attached clients. Either client must reverse the latest global
   action and both must refresh to the same accepted project revision.
5. Ctrl/Command+Z in a configuration text field or ordinary source editor must
   edit text. In the graph surface it must undo the latest project edit.
6. Save and restart: the project must restore its current durable state, with
   an empty session history. Package creation and other filesystem effects
   remain outside history.

### Durable settings history extension in progress (2026-11-13)

The owner accepted extending history to every persisted project setting:

1. Keep selected audio-device IDs owned by SystemAudioDevices, separately from
   hardware activation and availability. Record accepted IDs even when activation
   fails; return the failure as status rather than rejecting an accepted selection.
2. Add complete audio-selection and persisted toolchain records to the existing
   history/batch mechanism. PackageJit remains the toolchain owner.
3. Route audio selection through ProjectGraph's history coordination, with one
   visit to each affected owner and no NodeInstances/GraphConnections/GraphJit
   work for settings-only actions. Preserve audio.control authorization for changes
   and reversals affecting audio selections.
4. Restore settings through their normal owners. An unplugged requested output
   remains selected and unavailable, using the existing fallback. Toolchain undo
   affects future builds; it does not remove existing compiled artifacts.
5. Exercise normal edits, unavailable/failed devices, input/output atomicity,
   mixed graph/settings actions, no-op/redo, stale history, replay/save, and
   unchanged GraphJit/generation counts using fake backends and light tests.

No settings UI is requested. This records accepted direction and implementation
order, not a completed feature. Owner build/test execution remains unchanged.

### Settings-history source checkpoint (2026-11-13; owner verification pending)

The owner reported all tests passing for the graph-history slice. The accepted
settings extension above is now in source, with these concrete changes:

- Audio selection writes enter ProjectGraph and then SystemAudioDevices once,
  through `project_graph_system_audio_devices_bridge`. Reads stay direct.
- PackageJit settings use `project_graph_package_jit_bridge`; PackageJit retains
  canonical ownership and applies dirty loader settings on the next requested
  build. Existing loaded package revisions survive settings changes.
- ProjectEditBatch/ProjectEditEffect carry complete audio-selection and persisted
  toolchain values. Mixed actions capture one combined effect; settings-only
  actions skip all graph owners, compilation, and graph-generation publication.
- Activation and enumeration errors are returned in per-side audio status.
  Selected IDs stay accepted and saveable independently of those errors. Output
  fallback preserves Tick's clock; input and output activation are independent.
- Audio-specific authorization also applies to batch and history requests when
  they affect selection. Graph-only undo does not require audio.control.
- Explicit null overrides survive replay merging and are applied by the owner.
  The literal audio ID `"default"` is preserved. This corrects a pre-existing
  inability to round-trip those settings.

New light regressions cover availability and activation errors, atomic input/
output restoration, no-op hardware work/redo, stale settings and permissions,
saving despite enumeration errors, runtime-flag preservation, invalid future
build settings, mixed graph/settings validation and undo, replay reset, explicit
clears, parser duplicates, and the revision-stamped socket route. The transport
harness now uses real ProjectGraph audio history for those requests.

No agent build or test run is claimed. After rebuilding, run the C++ suite and
client checks as usual. Manual validation should include switching devices,
undo/redo after unplugging a selected device, and save/reload with disabled
input and a literal `"default"` output. No new frontend settings UI was added.

### Stage 3 start and verification update (2026-11-13)

The owner reports that all tests pass for the graph-edit history and its durable
settings extension. The earlier pending-verification checkpoints above record
prior states; this update supersedes their verification status.

The accepted Stage 3 order is:

1. Connect Vue manual controls to the existing backend hover, interaction,
   dynamic-input preparation, settled specialization, and grouped history paths.
2. Keep interactions in a host-owned context across component remounts,
   presentation-mode changes, and compatible graph revisions. End an interaction
   when its target disappears or becomes incompatible; release it on disconnect.
3. Generate frontend-owned TypeScript bindings from the language-independent
   node-event protocol and expose typed `node.send(event)`.
4. Exercise controls and typed events in a bundled specialized presentation,
   retaining the generic lane fallback.

The first coherent feature is manual control editing together with its durable
interaction context. A component must not own a server gesture that is lost when
that component unmounts. Live meters, waveform observation, and playback work
remain in their planned stages. No new settings UI is part of this feature.

Implementation is starting; no new build/test result is claimed. The owner
continues to run builds and tests.

#### Manual-control integration findings

The backend currently configures only explicit ProjectManualControl records.
Consequently a default-valued free input cannot be hovered or dragged without
first creating a durable record, which would make a frontend initialization
request appear as a separate undo action. The implementation will configure
eligible authored Sample defaults as implicit controls, retaining the same
constant specialization when idle. Only a changed manual value needs a durable
record. Hover and a no-op gesture must not create project state or history.
Eligibility and current stored values will be published in graph.getScope;
Vue must not infer eligibility from a type name alone.

#### Stage 3 module-input lowering friction (2026-11-13; unresolved)

The first implementation is in progress and has not been built or tested.
The backend's existing `ConfiguredManualControl.input` can name a public module
input, but `build_sample_port_binding_plan` in `graph_jit/lowering_plan.cpp`
requires that input to belong directly to a concrete Tick primitive. A subgraph
boundary has no entry in `analysis.primitives`, so it is rejected. Sequential
inputs of Tock leaves likewise cannot use that Tick-only lowering.

For example, `Module.cutoff` can be connected to a conversion and then to a
filter. Injecting its manual value into the filter instead of at the module's
public input skips the module's conversion. A module input may also fan out to
several consumers, each with its own other sources and combine operation.
Simply copying a scalar to each final input does not preserve those semantics.

The proposed next step is to lower the manual value at the public module input
and preserve its internal routing/conversions. An alternative is to deliver
direct Tick-leaf controls first and explicitly defer module inputs. This choice
has been sent to the owner; no decision is inferred from implementation ease.
Do not interpret the in-progress implicit-default bindings as verified support
for module or Tock inputs. The current working tree is an unfinished feature;
full suite success is not expected until this lowering issue is resolved.

Independent frontend work now in source includes a workspace-owned control
request queue, coalesced unsent values, panel-owned interaction identities,
webview-root pointer tracking, numeric inputs and scrub handles in generic
lanes, and semantic `useManualInput` access for specialized components. Tests
for these lifetimes and failure boundaries are in source but have not been run.

#### Module-input decision (2026-11-13)

The owner selected support for module inputs now. The feature therefore includes
GraphJit lowering of manual values at public module inputs, preserving internal
routing and conversions. A direct-leaf-only release is not the accepted scope.
The earlier friction record is retained to explain this expanded prerequisite.

#### Stage 3 manual-control source checkpoint (2026-11-13; owner verification pending)

This checkpoint supersedes the earlier unresolved module-input finding. The
accepted module-input extension is now in source. No agent build or test run
is claimed.

- `ProjectGraph` configures implicit authored defaults for eligible public
  Sequential Sample inputs. Hover and an unchanged gesture do not create saved
  records. A changed first gesture records absence-to-value as one history
  entry; undo restores the authored default. A round trip to that implicit
  default within an uninterrupted gesture removes its new record/history.
- `graph.getScope` exposes manual-control eligibility, availability, stored value,
  participation policy, and whether the value currently participates. Vue uses
  this metadata rather than guessing eligibility from a Sample type name.
  Direct Tock-leaf controls remain outside the existing Tick control backend.
- Structural routing retains module-input manual contributions through internal
  conversion, projection, nested boundaries, and detach operations. Concrete
  destinations receive those connection contributions before any direct leaf
  manual contribution uses the leaf's authored combine operation. No authored
  control nodes are inserted.
- Uniform settled module-input contributions can use immutable input bindings.
  Timeline and delay storage is planned before execution; constant/dynamic
  code variants retain the same NodeLayout. Compatible source/target route
  identities preserve their state through existing raw-region migration.
- The realtime root latches each semantic control's target once per audio
  callback, including fanout to several destinations and primitive slices.
- `WorkspaceSession.manualControls` owns queued control requests and interaction
  identities independently of Vue. Unsent pointer values coalesce, mutations
  use the latest acknowledged revision, and rejected mutations are not replayed.
  Closing a panel finalizes its last desired value; socket loss discards local
  gestures, while the backend retains the last accepted durable value.
- The graph webview owns pointer/keyboard gestures outside presentation widgets.
  Generic mini/editable lanes expose numeric fields and scrub handles. A mode
  change or widget remount does not end a drag. `useManualInput(name)` exposes
  the same semantic capability to future specialized components.
- Scope refreshes preserve compatible gestures and explicitly invalidate
  removed/incompatible inputs. Acknowledged values remain visible until the
  read model catches up; other clients' later edits and undo are authoritative.

Added source tests cover queue ordering/coalescing, late replies after
connection loss, owner isolation, rejected updates and initial grabs, removal,
component remounts, pointer bounds, RPC revisions, implicit-default history,
module channel conversion, ordinary-source participation, detach fanout, and
constant-to-dynamic execution using the same storage. The runtime regression
reuses the existing package fixture; it adds no package build fixture.

Owner verification:

1. Run the normal C++ build and test suite.
2. In `vscode/client`, run `npm run typecheck`, `npm test`, and the normal client
   build/install procedure.
3. In a generic mini/editable lane, edit an eligible input numerically and drag
   its scrub handle while audio runs. Undo should reverse one complete gesture.
4. Change vertical zoom/display mode during a drag, then release it. The final
   value must persist and undo once. Removing the target must end the gesture.
5. Exercise a module's public input and its internal routing. Connecting an
   external source in disconnected-only mode retains the stored value without
   adding it; disconnecting restores that stored value.

Remaining Stage 3 work is unchanged: generate frontend-owned typed node-event
bindings from the neutral protocol, expose typed `node.send(event)`, and exercise
both events and manual controls in a bundled specialized presentation. Stage 3
is not marked complete by this manual-control checkpoint.

#### Manual-control verification follow-up (2026-11-13)

The owner built the feature and reported 921 of 923 C++ tests passing. The two
failures identified test-fixture mistakes: an acyclic detach in the planner
fixture, and a shorthand Constant alias absent from the runtime fixture's
package definitions. Both fixtures now use real feedback cycles; the runtime
fixture explicitly uses its already registered Constant and FeedbackGain nodes.
Its immutable-input variant remains ordinary fanout without a temporal cut.

Review also found that manual-only contributions could disappear from the
ordinary detach validation after routing. A manual route now retains the
ordinary source bundles in its same detached expression, and validation checks
that those dependencies actually close a cycle. This retains the cycle proof
through target-projection normalization and rejects acyclic manual-only detach.
A light regression covers that rejection. These fixes have not been built or
run by the agent; owner verification remains pending.

The next owner run passed 923 of 924 tests. The remaining runtime fixture
failed during construction, before GraphJit: its host-side FeedbackGain
arguments had no finalized configuration type identities. Its explicit Constant
arguments had the same issue. The fixture now takes parameter identities from
each registered provider signature, following the existing
`configured_module_graph_with_arguments()` host-test pattern, and supplies the
exact C++ parameter types. Production argument validation is unchanged. This
fixture correction awaits the owner's build and runtime verification.

### Stage 3 typed-event implementation start (2026-11-13)

The owner reports that all C++ and TypeScript tests pass for manual controls.
The subsequent semantic split of the GraphJit runtime tests also passes.
These results supersede the pending verification notes above.

The next feature connects typed frontend events to semantic nodes:

1. Generate TypeScript types, factories, and codecs from the neutral event protocol.
2. Expose `node.send(event)` and validate the current address and exact event identity before dispatch.
3. Keep dispatch completion and failure reporting outside component lifetimes.
4. Add a gain/bypass example package and its bundled Vue presentation.
   The owner selected this example. Gain uses a manual input. Bypass uses a typed event.
5. Add client regressions and document the commands for owner verification.

The backend remains independent of TypeScript. The existing package sidecar and
`nodeMessages.getProtocol` remain the sources for event schemas. Live observation
is outside this feature. Implementation is in progress. No new build or test
result is claimed.

### Stage 3 automation decision (2026-11-13)

_Accepted direction. This supersedes the gain/bypass example selection above.
No automation implementation or new verification result is claimed._

The first specialized presentation will edit an automation curve. Its source
produces timestamped Sample-valued events. Other nodes can transform these
points before a replayable Tick node interprets them as a curve. The event
carries a value, while its existing event timestamp carries the position.
Holding values and interpolation are consumer behaviors. The exact C++ event
name and interpolation policy remain to be selected.

The owner accepts debounced semantic updates instead of audio-rate control-point
edits. Vue previews point movement immediately. The client coalesces unsent
updates and commits the final desired position when the drag ends. Audio uses
the latest accepted realization until its ordered replacement activates.
Continuous audible tracking of every pointer movement is not required. No fixed
debounce duration or reconstruction-latency guarantee is established here.

A gesture remains one undoable operation, including any intermediate debounced
updates. Existing manual-control gesture history does not establish that typed
node messages already support this grouping. That path needs investigation.
Queued requests must retain the latest desired edit without retrying rejected
mutations against a different revision. Interaction ownership must survive Vue
presentation remounts.

Points are ordered data rather than one graph input per coordinate. This avoids
requiring thousands of ports for an ordinary editable clip. The proposed
`additional_inputs()` and `additional_outputs()` remain separate, unimplemented
authoring extensions for variable lists of independently connectable ports.
They are not prerequisites for this automation representation.

Required implementation work includes:

- A dedicated Sample-valued event type in the existing event representation.
- Indexed lookup in sorted event storage, including neighboring events needed
  to interpolate a requested region that contains no control points itself.
  Neighbor lookup must respect logical Region membership. Circular timeline
  ordering and equal-position events require explicit handling.
- Safe ownership of instance-specific point data. Current concrete leaf values
  must be trivially copyable, and configuration pointers resolve to retained
  package globals. A leaf cannot simply retain an arbitrary point vector.
- Replay of requested regions without sampling or scanning the complete clip.
  Current replay traits reject non-sequential inputs, although the normative
  coverage design permits replay with prepared random-access dependencies.
- Authoritative point data for the presentation after reload, undo, or another
  client's edit. The current scope interface does not expose a curve point table.
- Frontend-owned event bindings and `node.send(event)`, followed by the curve
  presentation and regressions for editing, reconstruction, and sparse lookup.

Binary search can locate range boundaries in logarithmic time. Returning the
selected events still costs time proportional to their count. A curve evaluator
also needs the surrounding segment or neighboring points. Sorted storage alone
does not supply the dependency or lookup contract. Explicit cubic handles will
need additional data if they cannot be derived from the neighboring points.

#### Immutable construction-array decision and in-progress work (2026-11-13)

The owner selected an explicit immutable array for construction data instead of
arbitrary owning C++ objects in concrete leaf configuration. A module can own
the editable point list and message handlers. Graph construction freezes that
list for a leaf that reads it during evaluation. This is an ownership decision,
not a return to one port per point.

The first prerequisite is in progress. `GraphBuilder::immutable_array()` copies
pointer-free, trivially copyable elements. `ImmutableArray<T>` is a read-only
pointer/count value. `NodeConfigRelocation` retains its allocation through graph
copies. GraphJit lowers the allocation into immutable configuration data and
retains native backing storage for the realtime root. Archive reconstruction
restores its pointer from the retained data instead of persisting the address.
New ownership and archive regressions are in source. They have not been run.

`SampleEvent` and `EventTypeId::sample` are also in source, with the event-name,
connection parser, and runtime payload checks updated. Event lookup, replay
support, source and interpreter nodes, frontend bindings, and the presentation
remain unfinished. No build or test result is claimed for this working tree.


#### Tock point-source and Tick interpreter implementation (2026-11-13)

_Accepted decision and current implementation status. This extends the
checkpoint above. The owner has not yet built or run this working tree._

The owner approved a Tock source for the sparse control points. The curve
interpreter remains replayable Tick code. The distinction applies to the two
leaves, rather than forcing the editable module into one execution domain.

The new optional `builtin_packages/automation` package registers:

- `iv.automation.point_source`: a persisted Tock event Region containing
  `SampleEvent` values. The point list is frozen with `ImmutableArray<Point>`.
- `iv.automation.linear_curve`: a stateless, replayable Tick node with a Region
  event input and a sequential Sample output. It finds the neighboring points
  and samples only its current block or requested replay window.
- `iv.automation.curve`: a class module owning the editable point list. Its
  independent `AddPoint`, `RemovePoint`, and `MovePoint` message types reconstruct
  the module through the existing durable replacement mechanism. It exposes
  both the interpreted values and the underlying point events.

The first interpolation policy is linear. Events at the same position retain
stable order. The last event at a position supplies that position's held value;
interpolation approaching that position uses the first event there. An empty
point list, a position before the first point, or a position outside the bound
Region produces zero. After the last point, its value is held to the Region's
end. Point-source coverage includes the rounded sample position of `duration`.
These are the initial node's policies, not universal rules for Sample events.

`EventRegion::neighbors()` now exposes the last event at or before a position
and the first strictly later event in that same logical Region. Sorted event
storage uses binary search. Overlapping Regions keep independent event groups,
including across the signed index seam and unsigned wrap.

Replayable Tick leaves with Random Access inputs can supply forward and reverse
coverage callbacks. Forward publication for their Tick outputs is positional
`IndexCoverage`; it does not create authored Region identity. During evaluation,
replayed Tick storage follows the consumer's requested positions. Authored Tock
Regions and recorded Regions keep their complete storage and membership. This
prevents a short read of the curve from allocating samples for the whole clip.

The point source sets `ctx.output<"points">().max_event_count()` in
`constrain_ports()`. Background event storage starts with a small allocation and
grows as events are written, within the selected bound. Finite counts survive
conversion and fan-in when allocating sequential playback and replay event
buffers before execution. Sources without a finite count keep density-based
sizing. Tick callbacks still allocate no event storage.

The shared GraphJit package fixture now includes the automation registrations.
A new runtime regression demands two samples of a ten-hour, three-point clip.
It checks the narrow replay demand and the resulting interpolated values.
A second runtime regression invokes the registered add/remove/move handlers
after the builder session has ended and checks their complete replacement lists.
Light regressions cover immutable-array lifetime and archive reconstruction,
Sample event identity, duplicate timestamps, independent overlapping Regions,
index seams, empty startup data, callback selection, and count bounds through
conversion and mixed fan-in. No test result is claimed yet.

The specialized Vue presentation is still unfinished. It needs authoritative
point data after reload or undo, frontend-owned event bindings, `node.send()`,
and message gesture history. The optional package does not enter the default
builtin discovery root automatically. This checkpoint does not mark Stage 3
complete or claim that the curve editor is already available in the UI.


#### Replay-domain clarification (2026-11-14)

The owner reaffirmed that intrinsic replayability belongs only to Tick nodes.
`PointSource` stays a Tock producer. It uses ordinary Tock evaluation and retained
event storage. `LinearCurve` is the replayable Tick reader. Its forward/reverse
coverage callbacks describe availability and input demand; background replay
executes its Tick callback. Sharing propagation callback interfaces does not
make Tock execution replayable.

The owner's latest run passed 935 of 937 tests. The two remaining failures
identified a Tock-only propagation check in GraphJit lowering and missing module
handler metadata in the automation test's builder package view. Both paths have
been corrected in source. Lowering also explicitly rejects intrinsic replay on
a non-Tick primitive. This correction has not been built or run by the agent.


#### Unified output DSL work in progress (2026-11-14)

The owner requests mixed output declarations using
`g.outputs("value"_P = curve, "points"_F = points)`, and first-output defaults
that preserve event/sample kind. The source example in `automation.h` is the
accepted authoring interface and must be retained.

Implementation plan:

1. Preserve the requested kind in named `_P`/`_F` arguments, selecting the first
   output of that kind from a node reference.
2. Resolve bare node arguments from the first declared output across kinds.
3. Carry mixed public output requests through GraphBuilder and SubgraphBuilder,
   retaining declaration order in boundary `NodePorts::output_configs`.
4. Preserve per-kind connection indices, public channel families, and source
   annotations while ordering mixed declarations. Archive/import already carry
   the unified NodePorts configs and must retain the resulting order.
5. Add light regressions for event-first defaults, explicit kind selection,
   multiple outputs, mixed modules, subgraphs, and archive reconstruction.

At the start of this pass, the implementation separated Sample and event
output requests. The `_F` node-input path already carried event kind, but the
public `outputs()` helper routed every argument through sample lifting. This
was source behavior to correct, not an intended restriction on the owner's syntax.

The source now carries `OutputRequest` variants through both builders. Boundary
configs preserve mixed declaration order, and source annotations count each kind
separately. Bare node arguments and node chaining select the first declared
output; explicit sample/event contexts select the first output of that kind.
The initializer-list API retains the sample source configuration it preserved
before this change. No realtime representation or callback execution path changed.

Light regressions cover first matching outputs, typed and erased event-first
arguments, outputless chaining targets, mixed subgraphs and archive reconstruction,
and mixed initializer lists. The existing source-provenance integration test now
checks alternating event and sample public outputs, including each kind's second
output. The owner's `automation.h` syntax is retained. Build and test execution
remain with the owner; no result is claimed for this pass.

#### Shared live and replay sample producer correction (2026-11-14)

The owner's 943-test run passed every test except
`SparseTockPointsReplayOnlyTheRequestedCurveWindow`. Compilation rejected the
curve's replay consumer as a foreign producer connection. `SampleProducerGroupPlan`
correctly records both the live Tick connection and the background replay
connection for the same declared output. `build_sample_storage_plan()` incorrectly
required realtime delivery when validating that connection's source identity.

Source ownership now checks the declared producer port independently of delivery.
The existing delivery checks still exclude replay consumers from realtime storage
allocation. A light regression covers a shared producer, absence of a realtime
representation for its replay consumer, and rejection of a genuinely foreign
source. This correction has not been built or tested by the agent.

#### Application-owned toolchain environment (2026-11-14)

The owner identified the startup PCH/PIE mismatch as missing application build
environment activation. The temporary forced `-fPIC` change was reverted. The
accepted replacement is recorded in
[Application-owned compilation environment](application_toolchain_environment.md).
The source now captures and activates the installation's compiler environment,
checks compiler/PCH/plugin override compatibility, and restores installation
defaults when project overrides are cleared. Project persistence already filters
matching installation defaults; it retains that behavior. Owner verification is
pending. The separate clangd extension can use the generated local launcher.

#### Knob gestures and temporary input override plan (2026-11-14)

The owner accepted radial knobs beside input identities in the port UI. Ordinary
left-drag edits the independent stored manual value. Wheel input during a drag
changes sensitivity without changing the value at the current pointer position.
The default has extra mouse travel assigned to its exact value; no numeric
interval is clamped away, and this travel width is independent of sensitivity.
Right-click opens numeric entry (Enter accepts, Escape cancels). Double-click
resets to the declared input default. This replaces the original Hover + R plan
under the owner's 2026-11-14 decision. The
[earlier wording](./historical/knob_reset_before_double_click_2026-11-14.md)
is retained. A compact VS Code status item exposes the current context's
shortcuts and drag sensitivity, with fuller help in its tooltip.

For a connected input, a gesture temporarily replaces the incoming value. Ending
the gesture restores the normal connection when its compiled version activates;
the last manual value remains stored. Hover only prepares code. This is a
transient specialization, not a durable change to connection participation or
wiring. An unrelated graph compile preserves a valid active interaction.

Implementation order: complete backend connected-input overrides; add reusable
radial gestures, numeric entry, and reset; place the control once per input in
port popovers; expose contextual status hints; add focused regressions. Keep the
existing Shift connection gestures and defer output meters/observation to a
separate pass. The owner continues to run builds and tests.

Backend friction found before implementation: a connected module input using
`disconnected_only` produces no `ManualSampleRoutePlan` in the current structural
sample-route expansion. Starting an override would introduce a `ManualControlPlan`
and its persistent raw region. `GraphExecutor::compatible_realtime_code()` rejects
changes to steady storage, region counts, manual binding counts, and participation.
For direct leaf inputs, dynamic participation also changes the private input
materialization requirements. Simply enabling `dynamic` is therefore insufficient.

`lowering_plan.cpp` currently allocates each routed manual control's state as
`sizeof(ManualControlRuntimeState)` plus a power-of-two delay ring for every
routed channel when a delay is present. Reserving all potential connected-input
controls in the settled graph would preserve the existing code-swap contract,
but would charge that storage even while their knobs are unused. For example,
a stereo two-second route at 48 kHz and block size 256 rounds to 131072 values
per channel, approximately 1 MiB for one delay ring plus control state.

The alternative is extending prepared code activation to support variant-specific
storage while retaining the structural generation's state and ordinary connection
history. This is a broader executor/planner change, and its activation and migration
costs must be designed explicitly. No runtime fallback, permanent reservation,
participation-policy change, or replacement-storage redesign has been implemented.
The radial-control and status-hint UI has not yet been changed in this pass. Report
this tradeoff to the owner before selecting either approach, per the repository
workflow's requirement to expose unexpected storage and architectural costs.

Owner response: permanently reserving potential override storage is rejected.
Preparing additional storage on demand is acceptable, including an exception to
the one-buffer rule when justified. Before changing replacement storage, distinguish
logically required history from the current lowering representation.

Follow-up source findings: `ManualControlRuntimeState` contains only target and
effective scalar bits (8 bytes). Direct leaf controls already receive that region,
including inactive connected controls. A current-block override without history or
delay does not inherently require a persistent delay ring; callbacks requiring a
contiguous block may still require a private transient materialization. The
producer's shared output cannot be overwritten to supply one consumer's override.

The large allocation comes from `ManualControlPlan` combining scalar state with
per-route, per-channel delay storage. A delayed, changing manual source does need
past information, but duplicating its scalar history for each routed channel and
consumer is a representation choice. A consumer's declared input history must
preserve the values actually consumed during an override, including after release
until those values leave its history window. Storage cannot always be discarded
at the end of the pointer gesture.

Next design work should first reuse existing private input timelines and transient
storage where possible, then identify and prepare only the additional persistent
history actually required. Do not treat a change to the `participates` metadata
alone as proof that the physical storage must change. Do not bypass module-internal
delay/conversion semantics to avoid allocating history. The 1 MiB example above
is the cost of the current representation for a particular delayed route, not a
minimum cost of every connected knob.

Accepted investigation direction (2026-11-14): model a temporary manual override
as an internal producer replacing the ordinary source at the selected public
Sequential Sample input. This is planner representation, not a requirement for
an extra runtime callback or a durable project node. Module-internal routing,
conversion, and authored delay remain ordinary graph operations. Reuse existing
private input storage and realtime port-state migration; prepare additional
storage only when shared producer storage cannot serve the changed input.
Settled values remain specialization constants, and hover preparation must not
activate an override. Investigate this backend change before continuing the
radial-control UI work above.

A shared allocation sized for the largest history is an optional allocation
policy, not the semantic representation. Distinct input histories can remain
live after their gestures end, so simultaneous live histories cannot alias the
same bytes. Do not preallocate maximum histories for every potentially edited
input. The existing code-only replacement path rejects layout changes and does
not perform the structural generation's migration; joining these paths remains
implementation work, not an already implemented capability.

Owner performance constraint (2026-11-14): do not assume whole-graph storage
migration is cheap enough for a manual override. If existing storage cannot be
reused, measure activation separately from cold preparation: raw bytes copied,
node lifecycle moves, port-history transfer bytes, activation duration, and the
first subsequent Tick duration as graph size and affected history grow. Existing
`NodeStorage::Migration::commit()` copies raw regions and visits matching nodes;
using it unchanged can introduce graph-size-dependent realtime work.

Page-backed storage shared between variants is a proposed alternative, not an
accepted redesign. Investigate stable addresses or separately allocated override
storage before remapping the entire node allocation. Reusing physical pages does
not itself establish C++ object relocation, mutable ownership, or bounded edit
work. Graph-wide virtual contiguity is separate from each callback's contiguous
sample-span contract. No migration timings have yet been measured for this
override feature, and no source implementation has been changed in this pass.

Owner decision (2026-11-14): implement the straightforward migration path now.
Keep compatible manual-control variants on the existing code-only activation
path. When a variant changes storage requirements, compile an ordinary successor
from the already configured graph and use the accepted generation chain's node
storage and port-state migration. This may move unaffected node state at cutover;
accept that initial cost, then profile it in actual projects before introducing
page-backed state sharing or another allocator. Preparing a variant must not
activate it merely because a control is hovered.

Backend implementation prepared for owner validation (2026-11-14):

- `ConfiguredManualControl::override_connections` represents the temporary
  producer independently of durable participation. Structural routing replaces
  only the selected input's source; the authored connections remain available
  for restoration. Module conversion and temporal cuts remain in the route.
- Detach validity is checked against the authored graph before source
  replacement. Opening a valid feedback cycle during a gesture preserves its
  authored delay; an originally invalid acyclic detach remains rejected.
- Hover-only controls are excluded from the activated dynamic set while any
  gesture is active. Existing cancellation/request identities reject late
  results after a gesture ends or its client disconnects.
- `GraphExecutor::can_replace_realtime_code()` preflights layout and Tick
  invocation-slot/capture compatibility. A participation metadata change alone
  does not require moving storage when the persistent layout remains compatible.
- A storage-changing variant is compiled as a normal accepted successor from
  the configured graph, preserving node placements and message handlers without
  reconstructing them through `NodeInstances`. Existing node lifecycle migration
  and semantic port-state transfers preserve state and consumed input history.
  This uses ordinary background-generation preparation as well as realtime
  preparation; no separate allocator or page mapping was introduced.
- This first implementation performs a steady compilation for preflight, then
  a transition compilation when migration is needed. Transition compilation
  serializes project control events to keep its values and predecessor fixed;
  audio continues running the previous realization. Accepted successors follow
  the existing cutover ordering, including waiting for a predecessor's transition
  phase to finish. These costs remain candidates for profiling, not new latency
  guarantees. `IV_EDIT_TIMING` includes the storage-changing successor's cold
  compilation/staging timings; realtime migration cost is not yet measured here.

Regressions cover source isolation/restoration, module delay preservation,
invalid-detach rejection during override, stale storage-changing results,
cross-client hover isolation, migration scheduling, Tick invocation compatibility,
and consumed history across normal/override/restored execution while an unaffected
consumer keeps reading the shared source. Builds and tests remain owner-run;
only source inspection and `git diff --check` have been performed in this pass.
The radial knobs, popover placement, numeric entry/reset, and status hints remain
the next UI phase of the accepted plan.

Owner validation: all tests passed for the backend override/migration change.

Radial-knob UI implementation prepared for validation (2026-11-14):

- `ManualInput.vue` renders a radial control with vertical dragging and keyboard
  arrow adjustment. `NodePorts.vue` includes it once per eligible input in port
  popovers or the first row of a connection popover; single-input ports can open
  a popover as well. The generic lane no longer repeats controls in its viewport.
- `ManualControlGestures` owns sensitivity, the default-value travel region,
  optimistic values during end acknowledgement, and numeric-entry drafts.
  Wheel sensitivity uses incremental pointer movement; the exact default gets
  twelve extra pointer pixels, with progress preserved when sensitivity changes.
- `ManualValuePopover.vue` edits locally until Enter accepts; Escape or outside
  clicks cancel. Values must be finite and within the reflected input limits.
  Metadata refreshes preserve the draft while updating its limits/default.
- The initial Hover + R reset required keyboard focus in the graph. The later
  double-click decision replaces it. The extension displays contextual shortcuts
  and drag sensitivity in a status item, clearing it on focus loss or disposal.
- Manual wheel input is captured before timeline navigation. Port popovers stay
  open while their knob or numeric entry is active, and value-only scope updates
  do not close them. Existing wire handles retain their own pointer targets.
- Quick successive gestures from the same view serialize behind the preceding
  end, including when its begin reply is pending. Other views still cannot take
  over an input whose interaction is active or ending. Late acknowledgements do
  not rewind the webview's latest local value; confirmed undo remains visible.

Added client regressions for sensitivity changes, reversible default traversal,
limit reversal, exact/canceled/invalid numeric entry, reset focus rules, metadata
refresh, status hints, optimistic completion, and successive gesture ordering.
Builds, typechecks, npm tests, and browser validation remain owner-run. Source
review and `git diff --check` are the checks performed during this UI pass.

Manual validation should include a first knob grab after focusing another native
application; fine scrolling inside the default region; connected-input override
and restoration; numeric Enter/Escape while a port popover is open; ordinary and
Shift wire dragging beside knobs; and status clearing when another editor gains
focus. Use `npm run typecheck` as well as `npm test` in `vscode/client`, since the
Vue components are checked by the former.

Owner validation: all tests passed for the radial-knob UI above.

Hover-leave settling and pointer-lock changes prepared for validation (2026-11-14):

- Mouse release closes the undo gesture. `ProjectGraph` retains the dynamic input
  while the same client hovers that knob. Leaving requests settled code immediately,
  without a debounce timer. Disconnect also clears that client's retained input.
- Regrabbing reuses matching accepted code. Other settled constants must still
  match. Predictive hover cannot replace a required request to restore connections.
- `GraphExecutor` applies a successor's configured dynamic target after migrating
  storage. This preserves undo while a released knob stays dynamic. A newer live
  target still takes precedence.
- Each wheel event halves or doubles sensitivity. The default-value travel region
  keeps its fixed pointer distance. Mouse dragging requests pointer lock and uses
  relative movement. Release, Escape, focus loss, and disposal release the lock.
- Controls appear between port circles and names. Logical values have greater
  emphasis than normalized 0–1 values. The normalized readout requires finite bounds.

New regressions cover hover retention, regrabbing, stale constant values, disconnect,
required settling, migrated targets, discrete sensitivity, and pointer-lock lifetime.
The pointer-lock tests use an injected browser interface. They do not establish that
the installed VS Code webview grants pointer lock.

Builds, typechecks, and tests remain owner-run. Manually try a first grab after
switching applications, drag past screen edges, and release without leaving the knob.
The override must remain until the pointer leaves. Also try Escape and focus loss
during a drag, and undo after release while the pointer remains over the knob.

Owner validation: all tests passed for hover-leave settling and pointer lock above.

Double-click reset and height-dependent port display prepared for validation
(2026-11-14):

- `KnobClicks` recognizes two short presses on the same knob. Relative movement
  during a drag disqualifies that press from reset. This remains independent of
  native click targets, which pointer lock redirects to the editor. The R handler
  is removed, and status hints describe double-click reset.
- `NodePorts.vue` measures the available height below the lane heading. Each
  direction displays the ports that fit, in reflected order. If necessary, its
  final slot opens a popover for the remaining ports. Only those hidden ports
  contribute to the folded handle's connection state. Direct inputs keep their
  knob popovers, and direct connections use their visible port circles.
- `KnobPointerLock` serializes acquisition and release across rapid regrabs.
  Delayed grants and unlocks cannot terminate a newer drag. A rejected acquisition
  can retry when native focus arrives. Empty button masks on synthesized locked
  movement do not end the drag. Explicit release, Escape, and real focus loss
  still end it.

Added client regressions for layout capacity, order, overflow, double-click versus
drag, delayed lock transitions, and focus recovery. Builds, typechecks, tests, and
browser validation remain owner-run. Manually check direct-to-folded transitions
while zooming, direct-port connection choices, rapid double-clicks and regrabs,
and pointer confinement while moving and scrolling during a knob drag.

Pointer-confinement follow-up and audio inspection (2026-11-14):

The owner still reports a hidden pointer that can move out of confinement during
a slow upward drag. The previous acquisition/release race fixes did not establish
that native confinement works. Browser and desktop behavior remains unverified.

- Fixed `NodePorts.vue`'s window-boundary dismissal path, which previously ignored
  `editingControl`. Popovers now ignore pointer movement and boundary dismissal
  while their node has an active knob gesture.
- `GraphEditor.vue` consumes knob motion before other UI handlers and releases
  any remaining pointer capture after the browser grants pointer lock.
- `KnobPointerLock` detects changing absolute client coordinates while the browser
  reports a lock. It emits at most one such diagnostic per drag. The extension
  output also reports lock rejection and unexpected release, with browser version
  and lock state. No recording command or diagnostic menu action was added.
- Removed the blue drag outline. Idle, hover, and drag use progressively stronger
  yellow stroke/fill feedback. The dragged state follows the local pointer, rather
  than a remote interaction phase. Keyboard focus uses the same color family.
- Static inspection of `simple_sine` found the saved route `saw` to
  `q24_icosphere_pan` to `system_audio_output`. The source defaults to 220 Hz,
  both filter data files exist, and the baseline FIR has approximately unity
  gain at 220 Hz. This establishes the intended signal path, not actual live
  hardware output or successful activation of the graph.
- The existing `audioDevices.get` route distinguishes hardware availability from
  the silent fallback clock. `WorkspaceSession` now queries it asynchronously on
  connection and logs the selected output and activation error. The query does
  not delay initial graph discovery or mutate project state.

Added client regressions for fixed versus drifting locked coordinates and for
audio-output diagnostics. Tests and typechecks remain owner-run. Native pointer
confinement and the cause of the owner's silence remain open until the client
reports the live lock/device state. This status does not claim either issue fixed.

Inline knobs (2026-11-14):

- Directly visible eligible inputs render their knob beside the wire handle.
  Port fitting uses each row's complete height. Hidden inputs use folded port
  popovers. Directly visible inputs have no redundant knob popover on hover.
- Input and output columns retain separate positions. Narrow labels hide names
  and readouts before hiding the wire and knob targets. Connection-preview colors
  use the port-circle positions, not the width of the input column.
- The temporary pointer recorder and its Command Palette action were removed at
  the owner's request after the editor permission issue was resolved. Automatic
  pointer-lock diagnostics and the editor compatibility warning remain. The
  [previous checkpoint](historical/knob_pointer_recorder_removed_2026-11-14.md)
  retains the recorder's implementation and verification history.

Detached-window pointer lock resolved (2026-11-14):

- The owner confirmed that the editor backport fixed detached-window pointer lock,
  then installed a normal fixed VSCodium release. The editor package definition
  now lives in the owner's `/etc/nixos` configuration.
- Editor patch scripts, their tests, package definitions, the generated editor
  link, and the temporary test profile were removed from this repository.
  `vscode/client` continues to build the extension.
- The investigation and superseded status are preserved in the
  [historical host-workaround record](historical/vscodium_pointer_lock_host_workaround_2026-11-14.md).

Desktop editor compatibility warning (2026-11-14):

- Project activation warns about detached-window knob pointer lock when the
  desktop editor's VS Code core version predates 1.133.0. This release contains
  upstream fix #329240. Both VS Code and VSCodium expose that core version.
- The warning is also written to the Intravenous output channel. It offers the
  main-window fallback and does not block startup. Browser hosts and unrecognized
  custom version labels do not receive an unsupported-version claim.
- Added focused regressions for the release boundary, prereleases, newer editor
  versions, and unrecognized labels. Tests and client typechecks remain owner-run.

Silent-output investigation (2026-11-14; next owner run needed):

- Stage 3 remains active. Before the specialized automation presentation proceeds,
  locate the owner's missing audio using low-rate system-output probes.
- The application samples one block per second in `SystemAudioDevices` before
  submission and in miniaudio before its callback returns. A service thread logs
  every two seconds to the diagnostic file and `server.message` notifications.
  Render probes include the graph generation, Tick calls, and matched/unmatched
  device-binding requests. Both sample probes include peak, RMS, DC, and invalid
  sample counts. The audio threads perform no diagnostic formatting or I/O.
- The probes are enabled for this investigation. `IV_AUDIO_DIAGNOSTICS=0` disables
  them at server startup. Library users/tests default to no diagnostics owner.
- Added focused coverage for missing-graph silence, binding selection across sliced
  device blocks, signal statistics, and sampling frequency. This pass has not
  been built or tested. Root cause and its regression remain pending the live
  output. [Audio output diagnostics](audio_output_diagnostics.md) describes the log.

Silent-output localization update (2026-11-14):

- Owner probes show advancing device blocks but `render.generation=0`, `ticks=0`,
  and no output binding requests after the package build finishes. The audio graph
  is not executing; changing detune cannot fix that missing execution.
- GraphJit compilation diagnostics were retained by ProjectGraph but absent from
  its publication. `ProjectGraphPublication::execution_failure` now sends those
  failures through the existing root report to SocketRpcServer and the client.
  Package-build success is followed by the actionable graph-compilation failure.
- Removed the probe's service-thread reads of realtime-owned executor pointers.
  The render-thread generation observation remains the supported evidence.
- Added two light regressions for error preservation and notification ordering.
  The actual compiler diagnostic and its targeted fix await the next owner run.

Silent-output compiler fix prepared (2026-11-14):

- The next owner log identifies `GraphJit lowering has no resolved implementation
  for a concrete primitive`. GraphJit collected registered leaves but omitted
  internal arithmetic leaves created by expressions in the panner module.
- The package now exports all compiler records. GraphJit resolves internal leaves
  through their code key and exact declaration callback in the pinned revisions.
  This also supports private leaf types without public `IV_NODE` registrations.
  Package lookup adds no work to the audio callback.
- Added shared-fixture regressions for execution, identical keys in a different
  package, and an early diagnostic for a missing private record. The owner must
  rebuild, run tests, and restart the server to validate this fix. The probes stay
  enabled until the live output confirms graph execution and signal delivery.

Silent-output module routing fix prepared (2026-11-14):

- The next log gets past compiler-record lookup and reports multiple sources at
  a structural sample input. The oscillator module contributes 17 voices with
  repeated `g.outputs(...)` calls. Structural routing now preserves the sum.
- Explicit transform dimensions represent all producer channels. They no longer
  require a registered speaker layout for the combined producer list. Per-edge
  conversions, detach settings, and module manual-control policy remain retained.
  Large native sums use compiler-sized root scratch instead of a speaker-sized
  temporary array. Small sums keep their existing fixed array.
- Added planner and execution regressions for module output sums and input fan-in.
  The previous new fixture used an invalid public `node<Type>()` call. Corrected
  it to use the internal concrete-construction function. Its shared setup failure
  explains the owner's dependent tests marked “Not Run.”
- Builds, tests, and live signal confirmation remain owner-run. Stage 3 stays
  active, with the audio investigation ahead of the specialized presentation.

Audible output owner-confirmed (2026-11-14):

- After live rewiring, generation 3 produces audible audio. Render and playback
  probes report identical nonzero samples and one matched system-output binding.
- The saved signal path is unchanged apart from connection IDs. The earlier
  startup error rejecting multiple sources at a concrete target still needs a
  reproduction. Do not attribute that error to incorrect user wiring or mark its
  normalization behavior fixed based only on the successful live edit.
- Corrected the remaining shared-fixture setup error: scalar-literal shorthand
  required the shipped `constant` definition, absent from that fixture. The new
  regression now uses the fixture's existing registered constants explicitly.
  Test validation remains owner-run before continuing the specialized presentation.
