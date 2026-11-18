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

Source update, 2026-11-16: the unified API, compiler metadata, background migration,
and updated fixtures are implemented. Full owner verification is pending; see the
[state lifecycle source checkpoint](#state-lifecycle-source-checkpoint-2026-11-16).
The earlier source-status sentence above describes the 2026-11-12 baseline.

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
- Advanced event visualization catalogs and two explicitly requested future
  `Sample` views: (1) a **time-local, exponentially decaying amplitude-density**
  display with fine, optionally Gaussian-smoothed amplitude bins (about one
  bin per displayed horizontal pixel is a reasonable first target; UI quality
  selection may follow), then (2) **time-frequency content over time** via an
  STFT spectrogram or wavelet transform. These are different observations, not
  additional `level`/`value` meter scales. Their intended contracts live in
  [Vue Output Observation](./vue_output_observation_direction.md#deferred-sample-amplitude-density-and-time-frequency-views-owner-intent-2026-11-16).
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

### Owner UI refinement and revised remaining-work order (2026-11-16)

This note updates priority **after** the Stage 4 live-output observation
checkpoint and the reported 1,062-test verification, with further owner UI
requirements recorded later on 2026-11-16 for folding, clipboard/paste,
automation bounds/zoom, ruler-connected event snapping, and palette exploration.
It is a plan for future
work, **not** an implementation result or an instruction to begin all requested
features immediately. The normative requirements are in
[Vue Lane, Meter, Port-Control, Cable, And Automation Interaction Direction](./vue_presentation_refinement_direction.md),
[Project Playback And Presentation Grids](./project_playback_and_presentation_grids_direction.md),
and [Vue Output Observation](./vue_output_observation_direction.md).

1. **Finish and verify Stage 4's real live-observation path first.** Resolve
   the outstanding module-output capability/structured-clone verification and
   make channel identity correct end to end. A stereo `Sample` output must get
   two green, ruler-graded meters with readable dBFS level scaling. Provide a
   separate linear, unscaled `value` presentation mode (`0.5` halfway on a
   0–1 scale). The output meter must expand across **the full remaining lane
   width**, with distinct stereo channels and legible shared grading rather
   than a tiny fixed-width widget. Retain Stage 4's safe probe activation,
   coalescing and realtime cost requirements; verify actual client behavior before declaring this done.
2. **Complete coherent lane headers, controls and clipboard editing.** Treat
   each input knob and port as one control (knob left, port right, label right).
   If folding is required, the **last directly visible input keeps its knob**
   and becomes the expansion anchor; hovering it extends the header with
   hidden inputs in the **same knob/port columns**, without moving visible
   knobs or relocating the panel to an unrelated screen position. No permanent
   inline numeric readouts. Show independent min/max indicators with a stable
   knob arc. Provide `c`/`v` on hovered knobs through the OS text clipboard:
   **copy raw numeric values**, paste through the actual live-knob gesture,
   activate the prepared dynamic GraphJit variant, apply the value when active,
   commit one undoable edit, and precompute the next constant-specialized
   variant as if the pointer were released while still hovering. Preserve
   pending-activation honesty, text-input priority, and invalid-value
   reporting. Investigate transparent contextual cables and status-bar-first
   information without sacrificing labels, port dragging or accessibility.
3. **Complete automation's sample-accurate clip editor and value scale.**
   Draw the seconds/sample ruler **inside the clip**, aligned with the primary
   time axis, and quantize every live point-drag draft to project samples.
   Support independently optional min/max value bounds: two bounds give a
   linear vertical axis; one or neither gives a stable curved axis as on
   knobs; point edits never auto-fit the axis. Honor existing lane `nano`
   (covered time intervals only), `mini` (read-only curve), and `editable`
   (points, tension, menus, full editing) vertical zoom levels without adding
   clip-local zoom. Retain small filled points, generous hit targets,
   Shift+click deletion, right-click segment menus/creation and status-first
   information. These are independent of the later graph-event snap input.
4. **Add the top-ruler header's event snap input.** After basic sample-
   aligned editing, expose a **non-resizable header input** connected to graph
   event streams. Prefer a Coverage input with multiple incoming contributions
   and a dropdown selecting the specific cover to use for snapping. Define
   timestamp-only projection from every event payload type to `TriggerEvent`
   without inventing conversions between unrelated event payloads. Preserve
   selected-cover identity, bounded graph queries, unavailability reporting,
   editing history, and project sample-grid fallback. This is a whole
   graph/connection-planning, observation, ruler, and automation feature.
5. **Implement output gain end to end as a separate control/GraphJit feature.**
   Add unbounded default-`1` gain knobs for gain-capable output ports; specialize
   unity to **zero gain-related generated instructions** and implement non-unity
   execution, snapshots, history and transition semantics together. Test
   compiler output and runtime results. This must not be an always-on multiply.
6. **Explore port palette variants, then defer larger organization work.**
   Prototype equal-RGB-distance yellow/orange input and blue/purple output
   pairings, without prematurely assigning the variants to event/continuous
   versus computed/buffered roles. Keep palette exploration independent of
   graph connection semantics until its meaning is selected. Investigate
   cable opacity during the near-term readability
   work, but project-authored subgraphs and their collapsed external cable
   merging form a larger graph/presentation feature. Stage 5 Tock window
   observation, bounded live waveform improvements, playback controls,
   input-value observation, and shared buffered-page storage retain their
   existing prerequisites; do not build them as incidental workarounds for
   this UI checkpoint. Re-evaluate their relative order after the active
   Stage 4 and lane improvements are verified.

The ruler-header Coverage input and universal event-timestamp projection are
new connection/execution dependencies; they do not replace the already
available sample-grid quantization or require completing full Stage 5
waveform observation before basic automation editing. Clipboard paste must be
verified against the manual-control GraphJit variant activation protocol, not
just against a Vue input value change. Exact knob expansion geometry and
open/close pointer hit-testing must be exercised together with cable endpoint
placement. The color-code assignment remains exploratory.

**Future visualization sequencing (owner request, 2026-11-16):** after the
current meter/editor priorities and their necessary observation infrastructure
are working, implement the time-local `Sample` amplitude-density view as a
contained new registered observation/presentation feature. It does not require
an STFT or wavelets. Keep the time-frequency view later, ideally after bounded
window/waveform observation is established. Neither view is a prerequisite for
lane layout, automation, graph-authored snapping, or output gain. Their
aggregation must stay bounded and outside the realtime audio callback.

The earlier source checkpoints below preserve their dates and observed behavior.
In particular, the prior double-click-add/right-click-delete automation
interaction is **superseded intent**, although it may still be what source
implements until the editor is updated. No C++/Vue tests were run for this
documentation-only reprioritization.


2026-11-15 update: the accepted
[automation and suspendable-propagation direction](automation_and_suspendable_coverage_direction.md)
supersedes the cyclic linear automation checkpoint below. The implementation
order is recorded in [Automation And Background Planning Migration](automation_and_background_migration_plan.md):
requested-only Tock bindings, suspendable propagation and scheduling, then the
complete segment/coverage/manual-fallback/presentation feature. Variant event
ports and optional event projections do not block the initial one-type path.

Stage 3, started 2026-11-13. The owner has verified Stages 1–2, including
project history and durable settings history. Earlier checkpoints below retain
their original dates and are superseded by the later verification updates.

Current checkpoint (2026-11-14): generic lane wiring, live manual controls,
and typed frontend dispatch have passed the owner's checks. The accepted
construction-data read model, grouped semantic-message gestures, and bundled
automation presentation are implemented in source and await owner verification.
See [the source checkpoint](#automation-presentation-source-checkpoint-2026-11-14).
Live output meters and other Tick observation remain in Stage 4. The previous
checkpoint is preserved in [historical documentation](historical/stage_3_before_automation_presentation_2026-11-14.md).
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
playback state to `GraphExecutor`. The owner selected an optional builtin callback
structure for nodes that need playback state. Ordinary Tock clip sources do
not need it. **Later owner correction (2026-11-17):** the initial
neutral/empty Tick-input-on-pause plan quoted in the previous checkpoint was
superseded. The current normative playback contract preserves covered held
Samples, delivers Events once on an applied paused seek, and keeps Random
Access Region/Coverage views intact. The initial playhead does not depend on
adding a node-facing held-position accessor. This historical implementation
checkpoint does not claim those revised semantics have landed.

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

The default `builtin_packages/builtin` package registers these definitions
(promoted from the optional automation package on 2026-11-14):

- `iv.builtin.automation.point_source`: a persisted Tock event Region containing
  `SampleEvent` values. The point list is frozen with `ImmutableArray<Point>`.
- `iv.builtin.automation.linear_curve`: a stateless, replayable Tick node with a Region
  event input and a sequential Sample output. It finds the neighboring points
  and samples only its current block or requested replay window.
- `iv.builtin.automation.curve`: a class module owning the editable point list. Its
  independent `AddPoint`, `RemovePoint`, and `MovePoint` message types reconstruct
  the module through the existing durable replacement mechanism. It exposes
  both the interpreted values and the underlying point events.

The first interpolation policy is linear. Events at the same position retain
stable order. The last event at a position supplies that position's held value;
interpolation approaching that position uses the first event there. Time is
cyclic: interpolation runs between successive points in cyclic order, including
from the last point to the first across the global wrap, so one point is
constant and only an empty point list produces zero. See
[Cyclic automation semantics](#cyclic-automation-semantics-2026-11-14); the
superseded duration-bounded policy is preserved in
[historical documentation](historical/automation_before_cyclic_semantics_2026-11-14.md).
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
  output. [Audio output diagnostics](historical/audio_output_diagnostics_2026-11-14.md) describes the log.

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

Knob regrab and drag smoothness investigation (2026-11-14; unresolved):

- The owner reports two defects after all tests passed. Rapid repeated grabs of
  one knob sometimes highlight and drag another knob, both inline and in a port
  popover. Dragging sounds clicky; `iv.project.saw` frequency is an example.
- Regrab fix in the working tree, not built or tested: `KnobPointerLock.start()`
  ignores a press while a session exists and exits a lock it did not request;
  `changed()` exits a late grant requested by an earlier press. While a pointer
  drag is held, `ManualControlGestures.hover()` ignores other knobs and `scrub()`
  returns false. `GraphEditor.vue` releases the drag before clearing hover.
- Temporary server capture in the working tree, not built or tested:
  `AudioOutputCapture` and `audioDevices.captureDiagnostic` record eight seconds
  of the miniaudio callback output with per-block timing and write a WAV to the
  temporary directory. The owner-requested Command Palette action has not been
  written, so nothing calls the route yet.

_Implementation friction (observation, not a requirement):_ the realtime ramp
in `stage_manual_control_sample_timelines()` is correct within one root
invocation. Source inspection shows that new targets reach it late and in bursts:

1. **Planned assumption.** A live target update is a cheap, non-blocking store;
   the manual-controls direction says a relaxed atomic suffices and needs no
   RPC or queue handshake.
2. **Observed constraint.** `ProjectGraph::handle_project_update_manual_control_interaction()`
   holds `ProjectGraph::mutex_` for every pointer update. It rewrites the durable
   `ProjectManualControl::stored_value` and history through
   `set_interaction_value_locked()`, then raises
   `iv_runtime_project_state_changed_event`. The `manualControls.update` route
   has `mutates_session = true`, so each update advances the session revision and
   publishes `session.revisionChanged`. The client fires `graphChanges`, and
   `GraphEditorProvider.refresh()` re-reads `graph.getScope` and the definitions
   on the same ordered command queue and mutex, discarding reads overtaken by the
   next update. `ManualControlInteractions` sends one update at a time, each using
   the preceding revision. `compile_control_storage_change()` also holds the mutex
   across a GraphJit compile and staging, blocking updates for that duration.
3. **Complexity introduced.** Target cadence depends on scope-read and compile
   latency rather than pointer rate. Each late step is ramped over one root
   invocation and then held. Every pointer movement costs a durable write, a history
   update, a session revision, a replay-log entry and a full scope read per editor.
4. **Reach.** `ProjectGraph` manual interactions, the `manualControls.update` route,
   `SocketRpcServer` revisions, `ManualControlInteractions`,
   `GraphEditorProvider.refresh()`, `GraphExecutor` target delivery, and the
   multi-client display of an in-progress drag.
5. **Alternatives.** (a) Make in-gesture updates transient: no durable write,
   history update or session revision; latest-value delivery to a stable
   lock-free target, with `begin`/`end` remaining ordinary mutations and `end`
   committing the one history entry. (b) Keep durable updates but stop editors
   re-reading the scope for an update's revision. (c) Lengthen the audio-thread
   smoothing to hide irregular arrival. (c) treats a symptom; (b) leaves the
   mutex, the revision per update and the compile stall.
6. **Recommendation.** (a), followed by a measurement of the remaining cadence.
   A ramp-length change is a separate normative decision.
7. **Dependency change.** Resolve this before the specialized automation
   presentation, whose debounced point gestures use the same interaction path.

Owner decision (2026-11-14): alternative (a), extended. During a gesture, an
update is a transient latest value that every listener hears immediately. It
creates no durable write, history change, session revision or reply. Other
clients' knobs follow it through unreliable, throttled, coalesced notifications;
`end` commits the stored value and the gesture's one history entry, and corrects
every knob. When a client disconnects, the server ends that client's open
gestures at the last value it received from them.

Source checkpoint (2026-11-14; not built or tested by the agent). The normative
contract is [Live gesture values are transient](node_presentation_and_manual_controls_direction.md#live-gesture-values-are-transient);
the procedure is [Manual-control gesture](event_flows/manual_control_gesture.md).

- Each semantic control has one `ManualControlLiveTarget` cell, owned by
  `GraphExecutor` for its lifetime. A `begin` controls request returns it to
  `ProjectGraph`. `ProjectGraph::handle_live_manual_control_update()` stores into it
  under `live_mutex_` only, without `ProjectGraph::mutex_` or the accepted-chain
  guard. While the gesture is open, `GraphExecutor::tick_block()` copies the cell
  into the active generation's `ManualControlRuntimeState::target_bits` before the
  root latches it. `GraphExecutor::stage()` binds every generation to the same
  cell, so a successor staged mid-gesture cannot restore a stale seed.
- `ProjectGraphGraphExecutorControlsRequest::targets` replaces the single target,
  so one disconnect cause can end several gestures. An end writes the committed
  value into every accepted generation before closing the cell. A rebuild that
  invalidates a gesture releases its cell directly; that cause already enters
  `GraphExecutor` to stage. Interactions on implicit-default controls are no
  longer erased by `rebuild_locked()` merely for lacking a durable record.
- `begin` no longer writes durable state or raises the autosave event. `end`
  commits through the existing `set_interaction_value_locked()`. A disconnect
  commits each open gesture at its last received value and reports
  `SocketRpcClientDisconnectedRequest::project_state_changed`.
- `manualControls.update` is an id-less notification parsed by
  `parse_live_manual_control_update()` and handled on the reader thread as a new
  `IV_EVENT_SOURCE(SocketRpcServer, "manualControls.update")`. The routed
  request, `UpdateManualControlInteractionRequest`,
  `ProjectUpdateManualControlInteractionRequest` and their events are removed.
- `SocketRpcServer` relays `manualControls.liveValue` frames to other subscribed
  clients through `Client::live_frames`, at most every 33 ms per client, after
  reliable messages. `end` and disconnect discard the gesture's unwritten frames
  before the committing revision. `graph.getScope` reports `live` and `liveValue`.
- The client sends updates with `JsonRpcSocketClient::notify()` once `begin` is
  acknowledged, without `expectedRevision`, and never waits behind queued
  requests. `ManualControlInteractions::live()` merges other views' gestures with
  relayed values. Each webview shows them through `ManualControlGestures::liveValue()`.
- Requested removal: the periodic audio probes, their reporting thread and
  `IV_AUDIO_DIAGNOSTICS` are deleted. Their tests and document moved to
  [historical documentation](historical/audio_output_diagnostics_2026-11-14.md).
  `AudioOutputCapture` stays as the recorder's destination via
  `RenderConfig::output_capture`.
- New Command Palette action, "Intravenous: Record Knob Audio Diagnostic": it
  records eight seconds of the miniaudio callback output to a WAV in the server's
  temporary directory. Beside the WAV it writes a JSON file with each block's
  steady-clock time and when each gesture message was sent. Remove the action and
  `AudioOutputCapture` once knob smoothness is verified.

Tests added or rewritten: transient gestures and completion-ordered history;
disconnect commits; late updates after disconnect or removal; the executor live
target across a mid-gesture successor and after close; live-update parsing; server
relay and frame discard ordering; disconnect revision publication; client
streaming behind a pending request; live-value display; and recorder clock mapping.

Owner verification:

1. Build and run the C++ suite; in `vscode/client` run `npm run typecheck` and
   `npm test`, then rebuild and reinstall the extension.
2. Drag `iv.project.saw` frequency slowly and quickly. If the audio still steps, run
   the recorder while dragging and send the JSON path. The WAV and the
   timing together show whether values still arrive late or the ramp itself steps.
3. With two editor windows on the same session, drag in one and watch the other's
   knob follow, then settle exactly on release. Edit another node during a drag;
   the gesture must continue and undo must step over the drag as one entry.
4. Close a window mid-drag; the other must settle at the last dragged value.
5. Confirm the periodic `Intravenous audio probe:` lines are gone.

Knob recording analysis (2026-11-14; observations and source corrections):

- The earlier panner recording and its initial interpretation are preserved in
  [the historical analysis](historical/knob_recording_analysis_before_direct_oscillator_capture_2026-11-14.md).
  Its callback handoff gaps show stalls; they are not render-duration measurements.
  `LearnedHrtfSourceIcosphere::tick_block()` recomputes HRTF coefficients whenever
  its direction changes, including every sample of a dynamic manual ramp. That
  remains a separate performance investigation. No panner DSP policy changed.
- The owner confirmed that `freq` clicks while moving even with `iv.project.saw`
  connected directly to audio output. Holding it still is clean. Direction controls
  can stutter while held, so those symptoms must not be treated as one defect.
- The direct recording `/tmp/iv-knob-audio-90246e39-088f-44b3-9844-005d75a2ccd0`
  contains eight seconds of audio in about eight seconds of callback handoff time.
  Large waveform jumps coincide with callback boundaries while frequency changes.
  At a stored 265 Hz, the stationary signal has a strong component at about
  832.5 Hz, which is 265 times pi.
- `SineOscillator::sine()` accepts normalized phase, with two units per turn. Its
  table already converts that phase to radians. `tick()` multiplied by pi a second
  time. Besides the wrong pitch, removing complete normalized turns during a
  frequency re-anchor then changed the audible phase. The earlier statement that
  re-anchoring ruled out an oscillator defect was incorrect.
- Source correction: `tick()` passes `phase + phi` directly to `sine()` and uses
  named ports. Two light `SineOscillator` tests compare constant-frequency and
  changing-frequency output against an integrated sine reference, across phase
  wraps, holds and different callback partitions. Owner verification is pending.
- Executor correction: `ManualControlLiveTarget::completion_sequence` makes the
  next callback copy a gesture's final value even after the gesture closes. This
  repairs an older target copied by a callback that raced with the final commit.
  A regression simulates that stale copy after close. This does not explain the
  oscillator's in-motion phase jumps; it fixes an independent end-of-gesture race.
- The existing temporary audio-recording action also requests a bounded pointer
  trace from the graph webview. The JSON includes press/release events, selected
  and hit-tested controls, knob rectangles and pointer-lock transitions, with an
  epoch-to-server clock anchor. Pointer moves are throttled before DOM measurement.
  Audio and gesture evidence is still saved if the webview cannot return its trace.
  The wrong-knob grab remains unresolved until that evidence identifies the event
  target and lock ordering. The two audio-only recordings do not contain it.

Verification checkpoint: source inspected and `git diff --check` used; the agent
has not run builds, typechecks or tests. Rebuild the application and extension, run
`SineOscillator.*` and the existing executor/knob tests, then listen to a direct
frequency drag. Use "Intravenous: Record Knob Audio Diagnostic" if a wrong grab
recurs; this version writes the pointer trace alongside audio in the same JSON.


Cleanup and remaining investigation (2026-11-14; implementation status):

- The owner reports that the oscillator fix sounds smooth and that all tests pass.
  The transient transport remains useful independently: live values avoid the
  graph mutex, durable writes and per-movement revisions. The owner questioned
  whether to keep it after the oscillator fix; the agent recommends retaining it.
- `GraphExecutor` initially retained every live target for its entire lifetime.
  Its registry now holds weak references and removes expired entries on lookup.
  Execution generations and active gestures retain the cells they actually use.
  The existing executor test covers sharing across a successor and final delivery.
- Removed the socket-specific forwarding method for live updates; the bridge now
  enters `ProjectGraph::handle_live_manual_control_update()` directly. Removed an
  unused recorder include. Disconnect now clears the webview's remote live values,
  with a client regression for reuse of the same control identity in a new session.
- The remaining temporary recorder serves two unresolved defects. It is retained
  until they are diagnosed, rather than removed as part of the finished pitch fix.
  Periodic audio probes remain removed. User project and layout edits are untouched.
- In `/tmp/iv-knob-audio-15336e7f-64a4-4169-920f-65239ec41eb0.json`, release is
  reported at the original `freq` knob (about y=267). The next press is reported at
  the `detune` knob (about y=305), and its DOM target and hit test both select
  `detune`. The owner sees the cursor restored over `freq`. JavaScript gesture
  ownership alone therefore cannot explain that press: the delivered coordinates
  already select the other control. The trace now records screen coordinates and
  unlocked mouse movement as well. No corrective coordinate offset has been added.
- The owner also hears distortion after roughly one minute with the oscillator
  module connected directly to output. This remains unresolved. A light test checks
  a constant frequency at one minute and one hour, then ramps it, against an
  integrated sine reference. It jumps the callback index after initializing the
  real oscillator state, so it tests elapsed-phase arithmetic cheaply. It does not
  reproduce millions of JIT callbacks, device playback or graph transitions.
- Audit finding for later performance work: `GraphExecutor::tick_block()` currently
  scans all `ExecutionGeneration::live_targets`, even with no open gestures.
  Filtering on the structural graph's `configured.dynamic` would be incorrect:
  `stage_realtime_code()` can activate a dynamic variant with the same storage.
  Avoiding this scan should publish the needed target bindings with code activation,
  while preserving completion delivery. It must not silently discard controls that
  become dynamic through code replacement. No such optimization is implemented here.

The owner verified the preceding version. The cleanup, additional trace fields and
new regressions have only been inspected statically; the agent did not run builds,
typechecks or tests. `git diff --check` is the mechanical check used for this pass.


Follow-up evidence (2026-11-14; unresolved investigation):

- The owner withdrew the one-minute trigger: lowering `iv.project.saw` frequency
  to zero, then raising it, is the suspected trigger. The previously added long
  elapsed-phase test remains a cheap arithmetic regression, not a reproduction.
- `manualControls.end` is sent on pointer release, with the final value.
  `ProjectGraph` closes that interaction under `live_mutex_`, so subsequent values
  for its id are dropped. It retains dynamic code through `hover_retained_inputs_`
  while the released knob remains hovered. Hover leave requests settled code.
- The recording `/tmp/iv-knob-audio-443616ad-2a86-4ee5-9e86-cb815411d806.json`
  has about 8.44 seconds of wall time for eight seconds of audio. Event times must
  be mapped through its block timestamps rather than treated as audio offsets.
  After that mapping, the stationary regions are clean tones at 500, 525 and
  210 Hz. A fitted single-tone recurrence leaves residuals around 1e-8. The first
  zero hold is a constant -0.04837; the second is +0.02037. The 210 Hz tone follows
  the first zero hold, so this recording does not show a stuck frequency or extra
  tones. The first interpretation of fixed DC despite positive targets was wrong
  because it used wall time as an audio offset.
- A zero-frequency sine oscillator freezes its phase and therefore produces DC;
  zero frequency does not imply a muted output. No muting or DC-filtering policy
  has been added. A light regression checks a ramp to zero, phase hold, and ramp
  back up against the integrated sine reference.
- The cross-knob recording
  `/tmp/iv-knob-audio-73faa582-76f0-41a2-8555-50329b253b51.json` again reports the
  next press on another knob after release. It does not yet contain the newly
  added screen coordinates or unlocked movement. Diagnostic format version 2
  marks the current recorder; rebuilding and reinstalling the extension is needed
  before the next cross-knob capture.
- Idle polling cost is not measured. For N runtime bindings, the current loop
  performs two acquire loads per binding per root callback, plus pointer chasing
  and comparisons. At 48 kHz and 256 frames that is about 375*N atomic loads per
  second. A module's fanout can produce several bindings for one semantic knob.
  The recommended next optimization is to prepare the dynamic target bindings
  off realtime and publish them with the active code. It must also deliver a
  final target after gesture close; it cannot merely skip bindings with no open
  gesture. Measure callback time before and after on identical graphs.

The new zero-frequency regression and diagnostic format marker were not built or
run by the agent. No Git index changes or project-file changes were made.


Disconnected input cutover and compact target publication (2026-11-14;
implementation checkpoint, awaiting owner verification):

- The owner reproduced `GraphJit transition input suffix lost its producer group`
  by disconnecting the oscillator. `specialize_transition_plan()` treated a
  successor's immutable input representation as if it must have a producer group.
  Disconnected defaults do not have one. `lowering.cpp` now schedules a constant
  source's suffix mirror before its consumer, using the same copy operation and
  inherited history window. It does not invent a producer or discard history.
- `DisconnectingModuleInputPreservesHistoryAndStartsItsStoredValue` disconnects a
  public module input, retains the two consumers' previous .75 history, and checks
  that their new values become the stored .25 through cutover and steady execution.
- `ExecutionGeneration::RealtimeCode` owns both a compiled graph and the compact
  `LiveTargetBinding` list for that variant's dynamic controls. Both are prepared
  off realtime and published with one pointer. A constant variant has no bindings
  to poll. The list changes on same-layout code activation as well as structural
  generation activation, so the structural graph's old dynamic flags cannot cause
  missing or spurious polling.
- `completion_sequence` remains checked by dynamic bindings after a gesture closes.
  Releasing a knob while it stays hovered therefore still delivers the final target
  after an overlapping stale callback copy. The release/hover-leave split is intact.
- Code activation moves the previous `RealtimeCode` owner into a retired slot. The
  next cold code staging call, or normal generation reclamation, destroys it.
  Activation never destroys the old binding vector or drops its compiled-code
  ownership. The retired slot is cleared before another pending code is published.
  The executor regression checks both directions of code activation, final delivery,
  replacement of an unactivated variant, and retention until cold reclamation.
- `iv_manual_control_polling_benchmark` runs the current executor on one no-DSP graph
  and alternates between its compact path and the same path with a reproduction of
  the former idle all-binding scan. Both modes retain the same legacy binding data
  and call the same no-inline polling function (empty span for the compact mode).
  This isolates hot-cache polling overhead; it is not a before/after binary comparison
  or a prediction of a full DSP graph's saving. The default sweep covers 0, 1, 16,
  256 and 4096 runtime bindings, alternates order over seven trials, and reports
  each trial and median nanoseconds per Tick. No timing thresholds enter tests.

After the regular configure/build, run:

```sh
./build-release/benchmark/iv_manual_control_polling_benchmark | tee build-release/manual-control-polling.tsv
```

Use `--bindings N`, `--iterations N` and `--trials N` to narrow or extend the sweep.
No measured speedup is claimed yet. The agent did not run builds or tests.

Pointer evidence from format version 2:

- `/tmp/iv-knob-audio-b6ed0702-7e0f-4cad-a3ac-05aa7e8022a5.json` reports VSCodium
  1.143.17232, Electron 43.7.7, Chrome 150.0.7871.250 and pixel ratio 1.44.
- The first post-unlock mouse move has zero movement deltas, but both screen and
  webview coordinates jump. Examples: client deltas about (10.7,49.3) and
  (10.3,35.1), screen deltas about (16.5,72.4) and (15.7,51.4). The next press can
  then hit `detune` instead of the original `freq`. This places the mismatch before
  the knob's DOM handler; it does not establish which editor/browser/native layer
  supplied the incorrect position.
- The owner confirms that resetting editor zoom does not remove the defect.
  No coordinate-offset correction has been added: distinguishing a synthetic warp
  from genuine movement is necessary before changing target selection.


Owner measurements and deferred presentation requests (2026-11-14):

- The owner reports that all tests pass after compact-target publication and the
  disconnected-input cutover fix. The idle polling benchmark medians are below.
  These are hot-cache component measurements with no DSP, not full audio callback
  improvements. The zero-binding paired delta is noise, and ratios against this
  near-empty callback must not be presented as whole-engine speedups.

| Runtime bindings | Compact Tick (ns) | Tick plus former scan (ns) | Median paired saving (ns) |
| ---: | ---: | ---: | ---: |
| 0 | 16.59 | 16.94 | -0.50 |
| 1 | 16.35 | 16.83 | 0.30 |
| 16 | 16.37 | 22.41 | 6.33 |
| 256 | 16.68 | 123.26 | 106.37 |
| 4096 | 16.56 | 3159.23 | 3142.72 |

At 4096 bindings the removed scan costs about 3.14 us per callback in this
experiment, about 0.059% of a 256-frame, 48 kHz block period. Small graphs gain very
little; the benefit is removing growth with idle bindings.

Active work: report actual manual-control compilation requests in the extension
log, including queued/running/completed/superseded/failed outcomes and timing.
Report the measured endpoint explicitly: compile preparation or executor staging
is not evidence that an audio callback has activated the code. Hover, release and
hover leave remain separate events: release commits and prepares settled code;
hover leave permits its activation. The prior checkpoint wording is preserved in
[the historical note](historical/knob_settling_before_release_preparation_2026-11-14.md). Logging must not reintroduce per-pointer-update project work or audio-thread
notification costs.

Deferred owner requests; do not implement during this pass:

- A performance reporting page: queued and running rebuild counts, plus per-block
  processing time and remaining deadline headroom. The useful headroom is
  `frames / sample_rate - measured processing duration`; callback scheduling delay
  and underruns should be reported separately, and remaining deadline time is not
  the same quantity as process-wide CPU utilization.
- Optional per-node or per-region attribution. Instrumentation must be opt-in and
  absent from normal execution when disabled. Fused specialized code and inlining
  mean individual node times cannot always be separated without changing the
  executable; report that limit rather than inventing precise percentages.
- Port idle/hover styling should provide the same subtle feedback as knobs while
  preserving input-yellow and output-blue identities.
- Cable/popover stacking: a self-connection's cable can float over an input popover
  while its output endpoint is hidden. Prefer keeping popover chrome above cable
  curves, with incident cables revealed through the existing individual-port
  anchors. Consider fading a curve near an obscured endpoint if needed. The owner
  also suggested fading the popover around crossing curves. These are alternatives
  for later visual testing, not a settled opacity rule.

Stage 3 remains active; these reporting-page and port-style requests do not replace
its specialized automation-presentation work or the unresolved pointer-lock defect.


Control-compilation logging implementation (2026-11-14; agent static review only):

- Added `ProjectControlCompileStatus` and `graph.controlCompilation`. The existing
  compile worker publishes queued status records as its own source after releasing
  the graph mutex. No incoming RPC cause re-enters `SocketRpcServer`; no status
  publication runs on the audio thread or on each live pointer update.
- Requests now report queued/running, predictive preparation, executor staging,
  cancellation/supersession and failure. Storage-changing recompiles report their
  second compilation. Previously ignored failed GraphJit results now surface the
  first available diagnostic in the extension log.
- `ControlCompilationProgress` correlates request ids, tracks queued/running jobs,
  and formats the log. `WorkspaceSession` displays a separate compiler status bar
  without replacing package-build or knob-interaction status. Disconnect clears
  its counts and timer. The new topic causes no graph-scope refresh.
- The measured interval starts when ProjectGraph creates the compiler request and
  ends at candidate preparation or executor acceptance. It is deliberately labelled
  as such. RPC transit and subsequent audio activation are unmeasured. Predictive
  code can wait for a gesture between those two endpoints.
- Added light scheduler, RPC serialization and client-progress regressions. The
  scheduler test blocks one fake compile, cancels its hover, verifies supersession,
  then checks preparation and staging of a later request. These do not compile an IV
  package or put timing thresholds in tests.
- Installation warning: the owner saw Node DEP0169 after a successful VSCodium
  install. The client's own source and build script contain no calls to the legacy
  Node `url.parse`, `url.format` or `url.resolve` APIs. The exact installer/dependency
  call site is not established. A read-only extension-list invocation did not
  reproduce the deprecation. No dependency downgrade, warning suppression or editor
  patch was introduced. The upstream API notice is
  <https://nodejs.org/api/deprecations.html#dep0169-insecure-urlparse>.

The performance page, opt-in attribution, port hover styling and cable/popover
stacking remain deferred as requested. The agent has not run builds, typechecks
or tests for this logging pass; `git diff --check` is the mechanical check used.


Active change (2026-11-14): release-time settled-code preparation. Release commits
the final value and predicts the variant needed when that client leaves the knob;
that prediction does not activate it. Hover leave must reuse ready code or promote
an already pending/in-flight request. Reuse must compare stored constants as well
as dynamic-input sets. Active dynamic code remains available for re-grabs. The
existing structural transition path still applies when layouts differ. Light
scheduler regressions now cover ready reuse, leave during compilation, repeated
releases with different values (both prepared and still compiling), concurrent
held controls, release before dynamic activation, restoration of the original
constant, and storage-changing activation. Existing blocking tests now arm the
block before release, since release itself starts preparation. No build/test has
run for this change yet.

### Owner follow-up list, 2026-11-14

The accepted interaction requirements are recorded in
[Owner interaction requests](node_presentation_and_manual_controls_direction.md#owner-interaction-requests-2026-11-14).
They cover modified middle-button zoom, creation without a form for zero-argument
nodes, default display names, renaming, definition discovery, multiple selection,
lane ordering, deletion, default-value pins, a larger default travel region,
larger port handles, and more compact popovers with a single-port icon.
These are pending work, not changes implemented by this documentation pass.

The ghost-cursor defect remains active. It can trigger incorrect hover changes
and unnecessary GraphJit compilations after release. Treat that consequence as
part of the regression requirement, alongside the wrong-knob grab itself.

The owner also requests investigation of `q24_icosphere_pan` performance. Aim for
roughly a tenfold improvement, including responsive live input edits. Separate
package load, graph compilation, and dynamic audio execution in measurements
before choosing a change. If that improvement is impractical, investigate another
implementation of full spherical panning (the owner's “360d x 360d” requirement)
using the available datasets and research. No replacement algorithm or quality
tradeoff is selected. This work is also recorded under Front 16.

_Owner priority decision (2026-11-14):_ the `q24_icosphere_pan` performance work
follows Stage 6. The ghost-cursor fix comes first, then the remaining Stage 3
automation presentation, the `Node::State` cleanup, and Stages 4–6.

The owner reports that all tests passed for release-time settled-code preparation
and control-compilation logging. Those checkpoints are verified.

Ghost-cursor fix (2026-11-14; source checkpoint, not built or tested by the agent):

- Evidence, from `/tmp/iv-knob-audio-b6ed0702-7e0f-4cad-a3ac-05aa7e8022a5.json`:
  every pointer-lock exit is followed by a move with zero movement whose position
  is displaced in the drag's direction. Presses made before moving carry that
  position; two of them hit `detune` while the cursor was visibly over `freq`. The
  first real movement, for example `movement (-19,-74)`, returns the reported
  position to the visible cursor. Which host layer displaces it is not established.
- `KnobPointerLock` now reports the lock origin when a drag whose lock was granted
  ends, including Escape, host release and cancellation. `PointerLockExitGuard`
  classifies the unlocked events that follow. Moves without movement and boundary
  events are dropped; presses, releases and clicks are re-dispatched at the origin.
  The first move with movement ends the guard. `GraphEditor.vue` then replays the
  enter/leave transition from the origin's hover chain to the real position, because
  the browser's own boundary events for that movement were dropped. Focus loss ends
  the guard. It is registered before every other window listener, so port popovers,
  knobs and connection gestures never see a displaced event.
- Consequences: a press after release grabs the knob under the visible cursor; a
  displaced move no longer closes the popover that hosts that knob or changes knob
  hover. Neither sends the hover-leave that requested settled code, or the hover
  that requested a predictive GraphJit compilation for another knob.
- Light regressions cover the guard's classification, the hover-transition order,
  and lock-origin reporting. DOM re-dispatch in `GraphEditor.vue` is covered only by
  typechecking. The temporary recorder now traces `lock-exit-drop`,
  `lock-exit-retarget` and `lock-exit-settle`; remove it after the owner verifies
  this fix.
- Owner verification: the guard fixes the interactions, but CSS `:hover` still
  highlights other knobs at the displaced position until the next click. Emulating
  `:hover` per rule was rejected as too convoluted. Experiments come first:
  `requestPointerLock({ unadjustedMovement: true })` is unsupported in this
  VSCodium (Chrome 150, Electron 43, Linux) and was reverted. Its fallback request
  also produced a spurious denial from the first request's `pointerlockerror`.
  Next, the owner compares `--ozone-platform=x11` and `--ozone-platform=wayland`.
  If neither avoids the displacement, replace per-component hover handling with one
  shared position-driven hover unit that knobs, popovers and highlights reuse.
- Result: both `--ozone-platform=x11` and `--ozone-platform=wayland` still light an
  unrelated knob after release. Hover is therefore no longer taken from the browser.
  `webview/trustedHover.ts` (`TrustedHover`) is the one hover source of each webview.
  It hit-tests positions from the pointer moves it receives, plus re-tests after
  DOM, scroll and resize changes. It dispatches non-bubbling `iv-hoverenter` and
  `iv-hoverleave` and marks the hover chain with `data-hovered`. Like a browser,
  it freezes hover while the pointer is locked or captured. `GraphEditor.vue`
  installs it after `PointerLockExitGuard`, so dropped displaced moves never reach
  it; `App.vue` installs it for the side panel.
- Every native `pointerenter`/`pointerleave`/`mouseenter`/`mouseleave` listener
  in `ManualInput.vue` and `NodePorts.vue` now uses those events. Every `:hover`
  rule (knobs, port rows and entries, disclosure rows, the copy button and
  toolbar buttons) now selects `[data-hovered]`. New webview code must follow the
  same rule. The guard's hover replay is removed; `hoverTransition` moved to
  `src/hoverTransition.ts`, which `TrustedHover` uses.
- Not built, typechecked or tested by the agent. The mouse cursor's shape is
  still chosen by the browser at its own position; no API changes that.
- Review corrections (not built or tested by the agent): `blurManual()` clears the
  knob's semantic hover, so it now also calls `TrustedHover.reset()`. The next move
  enters its chain again, even within the same knob, and predictive hover
  compilation recovers without leaving and re-entering. `PointerLockExitGuard`
  passes touch, pen and touch-generated mouse events, which are not displaced and
  do not end the mouse's guard. `TrustedHover` (`src/trustedHover.ts`) and the
  event redirect (`PointerLockExitRedirect` in `src/pointerLockExitRedirect.ts`)
  now take injected environments, so light Node tests cover hover order,
  reset recovery, lock and capture freezing, content changes under a resting
  pointer, window exit, origin re-dispatch, touch/pen pass-through and replay loops.

Temporary recorder removed (2026-11-14; owner request after verifying the
ghost-cursor fix; not built or tested by the agent):

- Server: `AudioOutputCapture`, `RenderConfig::output_capture` and the miniaudio
  callback copy, `CaptureAudioOutputRequest`, the `audioDevices.captureDiagnostic`
  route, its event and bridge subscription,
  `SystemAudioDevices::handle_socket_rpc_capture_audio_output()`, and the capture
  owner in `app.cpp`.
- Client: the "Intravenous: Record Knob Audio Diagnostic" command,
  `knobAudioRecording.ts` (including its `formatVersion: 2` marker),
  `WorkspaceSession.recordKnobAudio()`, `WorkspaceRpc.captureAudioOutput()`, the
  gesture observer in `ManualControlInteractions`, the graph webview's pointer
  trace and its `knobTrace` protocol messages, and the recorder tests.
- The recordings analysed above remain dated evidence in this document.
- Also removed at the owner's request: the automatic pointer-lock messages in the
  Intravenous output channel (`graphInteractionDiagnostic`,
  `WorkspaceSession.reportGraphInteractionDiagnostic()`), and `KnobPointerLock`'s
  coordinate-drift warning with its `move()` position argument and test. The
  denied-lock note in the knob's status hint remains, as does the connection-time
  "Intravenous audio output" line, which the owner keeps as useful in normal use.
  See [Knob pointer-lock diagnostics](knob_pointer_lock_diagnostics.md).

### Stage 3 continuation, 2026-11-14

The owner verified the ghost-cursor behavior and asks to resume this stage after
its review fixes and cleanup. The next coherent feature is the automation
presentation, through the existing neutral semantic-event protocol:

1. Provide frontend-owned typed event factories and codecs, plus `node.send()`.
   Validate exact event identity and current receiver support before dispatch.
2. Expose accepted construction values through the application read model, so
   curve points survive reload, undo, and edits from another client.
3. Keep point-drag ownership and coalesced semantic messages outside component
   lifetimes. Group accepted changes into one project-history operation.
4. Register the bundled automation presentation and cover the complete path with
   focused client and backend regressions. Observation remains Stage 4 work.

This entry records the implementation plan, not completion. Builds and tests
remain owner-run. Unexpected ownership or transaction friction must be reported
before choosing a workaround.

### Typed frontend node dispatch, 2026-11-14

Source checkpoint. This completes the typed-event API slice of the continuation
plan above. It does not mark Stage 3 or the automation presentation complete.

- `generateNodeEventBindings.ts` reads a neutral compiler sidecar or runtime
  protocol snapshot. A frontend names file selects nominal identities and assigns
  export names. Generated bindings retain exact fingerprints and validate payloads.
  Display names never select an event. The bundled automation factories come from
  the compiler-produced shared-package sidecar. Regeneration instructions are in
  [the bindings guide](../vscode/client/bindings/README.md).
- `useNode()` supplies `node.send(event)` through the presentation host. The
  webview root owns requests across presentation remounts and mode changes.
  Disconnect rejects pending completions. A presentation without a stable semantic
  address receives an explicit error.
- Both webview hosts dispatch through `WorkspaceSession.sendNodeEvent()` and the
  existing `nodeMessages.getProtocol`/`nodeMessages.dispatch` routes. The session
  captures its revision before reading receiver support. It checks exact event
  identity and payload, then submits that revision. It neither retries stale edits
  nor carries a request across server startup. Backend validation remains final.
- The existing backend owns durable replacement, history, and project publication.
  This slice adds no backend route, C++ event schema, or realtime work.
- Light client regressions cover generated payload types, immutable payload
  snapshots, range validation, schema/export collisions, receiver removal, changed
  fingerprints, revision pinning, remount-independent completion, out-of-order
  replies, and disconnect. Generation also has a checked-in-output regression.

The agent did not run builds, typechecks, or tests. `git diff --check` is clean.
Owner verification from `vscode/client`: `npm test` and `npm run typecheck`.
Then resume the accepted construction-data read model and grouped point gestures
before registering the interactive automation presentation.

### Accepted point data and message-gesture history, 2026-11-14

The owner reports passing npm tests and typecheck for typed frontend dispatch.
The next feature uses accepted constructor values as the curve's authoritative
read model. `graph.getScope` reports the accepted definition and argument values,
separately from desired source text. A failed desired configuration must not
replace that accepted data. Reusable invocations retain their pristine cache-key
values for lazy reads, without copying another point array.

History friction: debounced messages can interleave with another client's edit.
An A→B drag, B→C external edit, and C→D drag cannot be combined into A→D without
absorbing the external edit. The owner selected **end conflicting gestures**:
close the current undo group before a conflicting edit, then reject further
updates from its old gesture. Do not silently rebase those updates. Disjoint
record edits can remain independent. Undo/redo and client disconnect must close
open groups before operating on history or releasing the client.

This records the accepted behavior and implementation work, not a completed UI
or a new verification result. The constructor read model and grouped message
path must be tested before the automation presentation is marked complete.


### Automation presentation source checkpoint, 2026-11-14

Implemented; builds, tests, and manual UI verification remain owner-run.

- `ConfiguredNodeInstance::read_construction_arguments` reads accepted values.
  Cacheable invocations reuse their pristine cache-key arguments and serialize
  lazily. Uncacheable invocations freeze supported JSON before provider code can
  consume the invocation values. An unsupported read returns a diagnostic without
  invalidating an otherwise valid construction. `graph.getScope` separates these
  accepted values and definition ID from desired configuration source.
- `node.gesture()` groups a sequence of typed messages. Each message still applies
  through the normal coordinated replacement path. `ProjectGraph` merges their
  `ProjectEditEffect` records into one history entry. A conflicting accepted edit
  closes the group before recording itself; later messages with that gesture ID
  fail. Merely beginning a second gesture changes no history. Undo/redo close open
  groups; disconnect closes that client's groups. No history rebasing or retries.
- The webview owns `AutomationCurveEditing`, pointer capture, pending requests,
  and debounced point drafts independently of presentation component lifetimes.
  Release flushes the latest position and closes history. A final draft remains
  visible until a scope read reaches its last accepted payload revision; ending
  the history group alone is not a newer point value. Errors restore accepted data.
- The bundled `iv.builtin.automation.curve` presentation shares the parent timeline's time
  window. Double-click adds a point, drag moves it, and right-click removes it.
  Visible-window lookup uses binary search and neighboring segments. This edits
  constructor data; it adds no audio-thread observation or sampling work.
- Automation ships in the default builtin package. To exercise the specialized
  editor, create a direct `iv.builtin.automation.curve` instance; see the
  [automation guide](../src/intravenous/builtin_packages/builtin/AUTOMATION.md).
  The owner requested builtin packaging and `iv.builtin` IDs on 2026-11-14;
  the prior packaging is preserved in [historical documentation](historical/automation_before_builtin_promotion_2026-11-14.md).
- Added light regressions for pristine accepted values, replacement/undo/failure
  reads, grouped history, conflict rejection, disjoint edits, disconnect cleanup,
  RPC authority/revisions, remount-independent queues, draft flushing, and sparse
  curve geometry. The agent has not run these tests.

Owner verification: the normal C++ build and CTest run, then `npm test` and
`npm run typecheck` in `vscode/client`, followed by point add/move/remove,
undo/redo, reload, and a conflicting edit from a second client. Only after these
checks should Stage 3 be called complete. The agreed `Node::State` lifecycle
cleanup remains before Stage 4.


### Automation builtin promotion, 2026-11-14

Owner decision: the dependency-free automation nodes and class module belong in
the default builtin package, under `iv.builtin`. Source now registers
`iv.builtin.automation.curve`, `iv.builtin.automation.point_source`, and
`iv.builtin.automation.linear_curve` in that package. The separate automation
manifest and entry were removed, and internal construction/replacement IDs and
the Vue presentation selection were updated. The shared JIT fixture still tests
the same implementations through its independent catalog. The builtin-loader
regression now checks the shipped definitions and three module-message handlers.
The owner reported passing C++ tests before this packaging change; this change
awaits owner rebuild and verification. There is no constructor-data or event
payload migration in this pre-alpha repository.

### Cyclic automation semantics, 2026-11-14

Accepted owner correction. The global timeline is
cyclic. Automation has no semantic beginning, end, or duration. The point list
alone defines the curve: interpolate between successive points in cyclic order,
including the segment from the last stored point to the first across the global
wrap. This uses the actual `GlobalIndex` cycle, not a repeating clip whose period
is the distance between the earliest and latest displayed points. One point
therefore defines a constant curve. An empty list keeps the existing zero-value
behavior.

Remove the explicit duration argument and the restrictions that pin points to
`[0, duration]`. A finite event-storage Region is an implementation representation
of the stored point set; it must not cut off the interpreted curve outside that
Region. Cyclic interpolation uses modular index distances.

The curve module sorts the points it receives, keeping equal times in their given
order. Owner decision, 2026-11-14: `PointSource` may keep rejecting unsorted input.

The specialized presentation supplies the initial construction data and manages
ordinary edits through typed semantic messages. Users should not need to type
constructor expressions for normal creation or editing. Raw configuration remains
available for advanced use. Displayed time windows do not bound the curve.

Implementation status: implemented in source; the agent has not built it or run
its tests, so it awaits owner rebuild and verification.

- `EventNeighborMode::cyclic` on `EventRegion::neighbors()` and
  `sorted_event_neighbors()`: when no event lies on one side of the position, the
  last event of the Region is held and its first event is next. Bounded mode is
  unchanged. `TickMaterializedEventInput`, the background storage views, and
  `PersistedDataStore::Snapshot::event_neighbors()` (including positional
  recordings split at the signed seam) forward the mode. A cyclic lookup may name
  a position outside the Region.
- `AutomationCurve(std::vector<Point>)` takes only the point list, validates finite
  times and values and unique IDs, and stable-sorts by time. `point_index()`
  accepts any signed second whose rounded sample position fits `[-2^63, 2^63)`.
- `PointSource::range()` stores one finite Region that excludes the largest cyclic
  gap between neighboring points; `tock_coverage()` writes its points in cyclic
  order from that Region's beginning.
- `LinearCurve` publishes coverage for the whole cycle as two half-cycle ranges and
  interpolates with modular distances from the nearer endpoint. Equal neighbor
  times, as for one point, span the full 2^64-sample cycle.
- `ProjectGraphScope` carries `sample_rate` (`sampleRate` in JSON). The frontend
  model uses `2^64 / sampleRate` seconds as the cycle and interpolates the
  visible window's edges across the wrap. `readAutomationCurve()` accepts only the
  one-argument form and sorts the points.
- The curve's `PresentationRegistration` supplies `initialConfiguration: "{}"`;
  `NodeEditor` fills it into an untouched creation form.
- Tests (not run): neighbor lookup wrap, equal positions, single and empty
  sequences, materialized-input wrap outside its Region, `LinearCurve` wrap and
  single-point blocks, construction validation and `point_index()` limits, an
  unsorted point list in the JIT window fixture, one-argument message payloads,
  and the frontend cyclic model. No JIT runtime regression yet exercises
  `PersistedDataStore` cyclic lookups on a positional recording.


### Automation verification and next dependency, 2026-11-16

The owner reports that the latest fixes pass and that connecting the automation
output works in the application. This is owner verification; the agent has not
independently rerun the suites. The earlier pending checkpoints remain above as
history.

The next stage dependency is the agreed `Node::State` cleanup: one authored state
type and lifecycle vocabulary, with Tick state in `NodeStorage` and Tock state
in background-owned storage. Complete safe background generation migration and
its tests before Stage 4 observation. Keep the existing background layout records
as ownership details; they do not imply a second authored state type.

The playhead and play/pause feature remains unimplemented. Its contract is in
[Project Playback And Presentation Grids](project_playback_and_presentation_grids_direction.md).
The owner reports this limits playback testing; it must remain visible in the
remaining work rather than treating the working automation connection as playback
verification. General buffered-page storage remains a separate later feature.


### State lifecycle source checkpoint, 2026-11-16

The supporting cleanup is implemented in source:

- `Node::State` is the only authored state name. The primary callback family
  selects realtime or background storage. Legacy `TockState` declarations are
  rejected, with no compatibility alias.
- `InitializationContext<Node>`, `MoveContext<Node>`, and `ReleaseContext<Node>`
  use `state()` in both domains. Tock move also exposes `previous_state()`.
  Realtime storage facilities are unavailable in Tock initialization contexts.
  Tock coverage callbacks and batch lanes use `state()`.
- Compiler metadata records the single authored State. Finalization binds it to
  its domain-specific ABI. Internal background layout and storage records remain
  separate, and Tock state adds no realtime allocation.
- Background generation cutover offers a drained predecessor to an authored
  move by stable identity and compatible type/layout. An incompatible or
  anonymous node, a node without move, or a privately prepared successor starts
  fresh. Predecessor resources and package code stay owned until release.
- Regressions cover reflected aliases, generated scalar/batch access, storage
  separation, compatible migration across reload, incompatible/fresh fallback,
  and background-thread construction, migration, release, and destruction.

Targeted syntax checks passed for the changed implementation, the compiler
plugin and finalizer, the affected test translation units, and the complete
shared package source. The initial tool checks needed explicit LLVM/Clang header
paths because these Nix environment paths are absent from compile_commands.json.
The `IV_NODE` static authoring-validation suite also passed. No full build or
runtime test suite was run by the agent. The normal release build and CTest run
remain required before this cleanup is verified.

Stage 4 live Tick observation follows this cleanup. Playhead/play/pause remains
a separate unimplemented feature under its accepted contract; it does not depend
on completing all observation operations. The unbounded-knob magnitude design
also remains undecided, as recorded above.


#### Owner build and watcher-resource failure, 2026-11-16

The owner rebuilt successfully and ran 1,042 tests. The 11 failures all report
`inotify_add_watch(...): No space left on device`; the new State lifecycle
regressions passed. A serial rerun reproduces the same 11 setup failures. A
separate one-watch check on `/tmp` also fails with `ENOSPC`, outside the test
watchers.

The current user limit is 524,288 watches. Two Codium processes each expose
approximately 261,000 watch entries, and `build-release/` contains 272,683
directories. The workspace settings now exclude generated build and client output
directories from editor watching. Application dependency/discovery watchers are
unchanged. Reload the editor workspace to apply the exclusions if necessary,
then rerun the failed tests. Full verification remains pending those results.


#### Stable test workspaces and resource recovery, 2026-11-16

The owner rejected process-ID-based test workspaces and requested a build-tree
layout based on test structure. Mutable fixtures now use executable/suite/test/
fixture paths. A shared GoogleTest main resets the current case's directory before
execution; fixture paths no longer contain source-line numbers. The latest files
remain available after the run. Shared CTest package fixtures retain their
existing cache and locking behavior. See [Test Workspaces](../tests/README.md).

The agent removed 28,029 abandoned PID-named workspaces, conservatively keeping
14 names whose process IDs were in use. The one-watch check now succeeds, and
all 11 previously failing tests pass against the existing binaries. This confirms
the watcher setup failures came from exhausted file-watch capacity.

The new shared main and path scheme passed a compiled repeated-run and parallel-
case check. Test-support syntax checks pass, and four regressions were added for
the stable hierarchy, reset behavior, fixture names, and escaped-name identity.
The full CMake reconfigure, build, and test run are still needed to verify the
updated test infrastructure in the complete project. The temporary exit-cleanup
implementation and cleanup command were removed from source.


### Stage 4 start and verified prerequisite, 2026-11-16

The owner reports that all tests pass after the State lifecycle migration and
stable-workspace correction. The supporting cleanup is verified. Stage 4 is now
active: live Tick observation, with a numeric State field and a Sample output
level meter as the first complete feature.

Implementation order within that feature:

1. Trace the existing realization, control-code replacement, transport, and Vue
   presentation paths. Resolve source/type capabilities and bounded capture at
   the producer boundary without changing authored graph semantics.
2. Carry one accepted observation requirement snapshot through every structural
   and control-code compilation. Publish capture bindings with actual executor
   activation identity, including empty plans and source invalidation.
3. Add the Observations application module, scope reconciliation and lease
   cleanup, shared capture, bounded collection, and disposable per-client batches.
4. Connect host-owned Vue interests to generic output meters and one supported
   Tick State field. Keep subscription lifetime independent of component remounts.
5. Test edits, control-code races, activation, source removal, disconnects, and
   slow clients. Measure compilation and realtime capture overhead. Only then
   extend the working live pipeline with bounded waveform observation.

These are implementation steps of one end-to-end feature, not standalone module
deliverables. The output-observation contract remains authoritative. Playhead/
play-pause and background window observation remain separate outstanding work.


### Stage 4 live observation source checkpoint, 2026-11-16

The first live path now connects the Vue default lane, `Observations`,
`ProjectGraph`, GraphJit, `GraphExecutor`, and disposable socket notifications.
It supports `Sample` output `level` and reflected Tick State `float`/`double`
field `latest`. Source capabilities include operation result schemas; unsupported
field types and Tock State are not exposed. The default lane shows output meters
and the first supported State field. Specialized presentations can retain their
own lane contents.

`level` reduces the latest captured audio block, including its primitive slices
and selected physical channels. It reports sample peak, RMS, finite sample count,
and nonfinite count. It does not promise peak hold, true peak, or statistics over
blocks skipped by disposable delivery. State capture runs after the owning
primitive's callback. No mutable DSP storage is read by the control thread.

The compiler adds read-only observation inputs through normal connection and
storage planning. These inputs extend source-buffer lifetimes where needed.
Authored graph records are unchanged. Structural and knob-code compilations carry
the accepted requirement revision; obsolete control compilations cannot stage a
plan that drops newer probes. Executor frames carry the actual activation order,
including empty plans and unavailable playback blocks.

The graph webview owns one viewport scope. Visible presentations contribute to a
batched desired set, with a 200 ms grace period. Identical interests share capture
across clients. Scopes renew every 10 seconds, expire after 30 seconds, and are
released on disconnect or panel closure. The initial limits are 64 scopes, 64
interests per scope, and 256 distinct executable probes. A single control-path
collector uses the existing runtime service; no observation thread is added.
Updates are capped at 20 Hz and replace a queued batch for that client. They do
not enter the reliable project-revision journal.

For timing, `observations.reconcile` reports `compilationUs` for observation-plan
changes. Start the server with `IV_OBSERVATION_TIMING=1` to include `captureNs`
in `observations.updated`; clock calls around captures are disabled otherwise.
These measurements cover capture work, not all DSP or the extra source-buffer
lifetime. Compare the existing edit/audio benchmarks with the viewport closed
and open before drawing a conclusion about total overhead.

Verification is in progress. The next extension within Stage 4 remains bounded
live waveform observation. Stage 5 background-window queries and the playhead
contract remain outstanding.


Owner verification update, 2026-11-16: 1,060 of 1,062 tests passed. The two
failures exposed mistakes in the new observation tests: an RPC address omitted
its required empty path, and the source-removal test compiled a steady successor
without its predecessor cutover. Those fixtures are corrected; verification of
the corrections is pending. The RPC test also now expects the route registry's
exception when a method name disagrees with its parsed payload type.

Builds, test execution, profiling, and other slow commands remain owner-run.
The agent changes source and reads the resulting logs.


### Owner-requested connection cleanup on deletion, 2026-11-16

Pending implementation: when the user deletes a node, remove its incident
connection state, including connections to its children. Do not leave dangling
endpoints in the UI for the user to clean up. Preserve unrelated connections.
Apply the cleanup within the same undoable project edit, so undo restores the
node and its affected connections together.

This supersedes the earlier requirement to retain dangling matchers after
explicit user deletion. It does not request removing connections merely because
a definition fails to load or an endpoint is temporarily unavailable. Before
implementing the change, update the conflicting deletion passages in
`project_undo_redo_and_backups_direction.md`, preserving them under
`docs/historical/` as required by the development workflow. No deletion behavior
is changed by this note.

Owner verification update: all 1,062 backend tests pass after the observation
fixture corrections. The main graph's missing meters exposed a frontend gap:
reactive Vue addresses were passed directly to `postMessage`. Observation
requests now copy plain JSON data before that boundary, and RPC wrappers ensure
root addresses carry `path: []`. The client regression now uses reactive nested
addresses and a structured-clone boundary. Owner verification of this client
correction is pending.


### Missing module meters and deferred startup ordering, 2026-11-16

The owner confirms that the missing visualization is in the main graph window.
The default lane's meter component is present, but capability discovery rejected
module output declarations whose allowed value-type set contained more than one
type. `GraphBuilder::outputs()` deliberately uses the audio-capable type set;
that set is a declaration domain, not the port's resolved runtime type. A Sample
output such as the one exposed by `iv.project.saw` was therefore omitted.

The lookup now uses `ResolvedPortRealization` for those declarations. Discovery
reuses the configured generation's compiled type metadata when present. If it is
not present, the existing constraint solver can determine types without LLVM code
generation or a live audio node. Observation requirements use the same rule.
An output resolved to another value type is not forced to Sample by a meter.
Added regressions cover module capability discovery without an audio compilation
and module-address discovery through capture in a real compiled graph. Owner
verification is pending; no builds, tests, or profiling were run for this fix.

The owner explicitly defers the broader startup-order cleanup until the main
meter works. Record that follow-up as: after the necessary packages are built,
construct presentation structure from available declarations and configuration
metadata. Inputs, outputs, and visualization layout must not require live DSP
instances or the first successful audio-graph compilation. Pending live data
should affect the displayed value/availability, not whether those UI structures
exist. This pass does not claim to complete that broader lifecycle change.


### Observation fixture correction and refinement review, 2026-11-16

The owner reports 1,063 of 1,064 tests passing. The remaining module-meter test
failed before observation discovery: `make_graph_jit_snapshot()` published only
leaf definitions, so ProjectGraph could not create its requested module. The
shared fixture now publishes the revision's module definitions as well, retaining
their providers, configuration graph, metadata, and module lifetimes. The test
asserts the module is present before creating it. Owner verification is pending;
the agent has not run a build or test.

Reviewed the three owner-applied documentation patches dated 2026-11-16:
`intravenous_vue_ui_refinement_docs`,
`intravenous_vue_automation_ruler_snap_meter_followup_docs`, and
`intravenous_vue_nested_ports_clipboard_snap_and_curve_docs`. Their combined
remaining-work order is recorded near the start of this document under
[Owner UI refinement](#owner-ui-refinement-and-revised-remaining-work-order-2026-11-16).
That order takes precedence over older checkpoint statements about immediately
adding live waveform observation.

The current meter implementation still aggregates selected channels into one
`level` result, uses a short blue linear-amplitude indicator, and has no signed
sample `value` operation. These are implementation gaps against the new channel-
correct green dBFS/linear-value contract, not accepted compromises. Complete and
verify the default lane's visible meter path before expanding controls or ruler
snapping. The owner's broader startup metadata-order correction follows the
missing-meter repair, as already recorded.

The automation sample-index requirement also reaches beyond drawing a grid:
`automation::Point` and `MovePoint` currently transport floating-point seconds.
The accepted exact integer sample identity must therefore be preserved through
typed messages, saved construction data, generated bindings and live drafts.
The later ruler Coverage input additionally needs contribution identity and the
specific event-to-trigger timestamp projection. Neither requirement calls for
a new replayable Tock mode or an implicit project BPM system.

Explicit user-deletion connection cleanup remains pending alongside the earlier
owner request. Palette semantics, ruler ownership and snap interaction details
remain decisions for their respective features; do not silently choose them
while implementing the current meter fix.


### Channel-correct meter implementation started, 2026-11-16

The owner reports all tests pass after the definition-fixture correction and
accepts the revised order. The next complete feature is default-lane meters:
explicit per-channel interests and capability names, separate registered signed
sample-value capture, shared ruler grading, full-width green dBFS meters and
linear value mode. Mode changes must replace observation interests without
changing the authored graph; unavailable values remain distinct from silence.
Meter context goes to the existing status item. Tests and source review follow
the final feature shape; builds, test execution and profiling remain owner-run.
Startup metadata ordering and the later control/automation features are not
part of this meter implementation.


### Channel-correct meter source checkpoint, 2026-11-16

The default `NodeLane` presentation now groups each Sample output's advertised
channels and renders one `SampleMeter` per channel. `observations.describe`
returns channel indices/names and the registered operation schemas. Each live
interest selects a channel explicitly; an unselected multichannel source is not
silently reduced to mono. Shared captures still require matching source,
channel and operation. Viewport probe budgeting keeps a stereo pair together.

The existing Sample `level` operation captures peak, RMS inputs and finite/
nonfinite counts independently per selected channel. The new registered `value`
operation captures the final signed sample of the current block, including
primitive slices, with no logarithmic or magnitude transform. Nonfinite scalar
values serialize as null and are unavailable to the value display.

Level rendering uses a green RMS fill and sample-peak marker, with a default
−60 to +6 dBFS domain. Zero is shown below the floor; levels above unity remain
visibly above the 0 dBFS reference. Value mode uses a linear 0–1 domain, retains
signed values, and explicitly labels values below/above that display range.
Meters fill the flexible lane remainder, have square corners, and use the same
`RulerGraduations` component and numeric grading geometry as `TimeRuler`.
Short lanes keep separate channel bars and suppress labels that cannot fit.
Unavailable results have a distinct empty-pattern display, not a measured zero.

The small output mode control switches `level`/`value` by reconciling observation
interests. Operation identity participates in each frontend probe ID, so an old
peak result cannot appear as a signed value while replacement code is pending.
The choice survives component remounts and is saved in webview-local state;
it is not a project mutation or undo entry. Meter hover/focus context uses the
existing VS Code status item; meters no longer use native title tooltips.

Added source regressions cover wrapped/interleaved per-channel reduction,
signed final samples across slices and ring wrap, stereo capture through the
executor, channel capability names and schemas, channel/operation sharing across
clients, frontend mode replacement, complete-pair budgets, persisted frontend
choices, availability, dBFS/linear scaling, and shared ruler geometry.

No builds, test execution, profiling or interactive visual verification were
performed by the agent. The owner should rebuild the server/tests and reinstall
the client, then run CTest and the client typecheck/tests. Visual checks should
include a stereo output with unequal channel levels, a negative value-mode
signal, silence versus unavailable data, meter mode changes while audio runs,
and lane zoom down to the smallest height. Runtime cost still needs owner-run
measurement against the existing observation timing/benchmark paths. The
broader startup metadata-order correction remains pending after this meter
feature is verified; Stage 4 is not declared fully complete here.


Owner verification update, 2026-11-16: 1,068 of 1,069 backend tests passed.
The stereo capability fixture changed only `ModuleNodeDefinition::provider`,
while the package's `provider_definitions` still invoked the mono callback.
`NodeInstances` configures through that package record. The fixture helper now
accepts a construction callback and installs it in both records; the regression
also checks that the configured output is stereo before checking capability
names and schemas. Verification of this fixture correction remains owner-run.


### Shared meter readings and smooth peak motion, 2026-11-16

The owner reports that the meter works, then requests faster refresh and the
same peak/RMS display in both modes. This supersedes the earlier source
checkpoint where value mode used the final signed sample. The removed design
wording is preserved under `docs/historical/`, with links from both normative
meter documents.

Current timing before this change: DSP capture occurs every block; collection
has a 50 ms gate checked by a runtime loop sleeping 20 ms. That quantizes idle
collection to roughly 60 ms (about 16–17 fresh readings/s), before processing and
transport delays. The socket writer additionally caps disposable frames at
33 ms. Unchanged values may be suppressed. The reported perceived rate is not
a measurement of DSP execution frequency.

The collector now targets approximately 30 readings/s with a deadline that
retains its phase across the existing runtime poll, skipping missed intervals
without catch-up bursts. The socket's existing coalescing and reliable-message
priority remain. This adds control-path collection/serialization opportunities;
it does not increase DSP capture frequency or add a worker thread.

Both meter scales now keep the same per-channel `level` subscription and peak/
RMS data. Switching scale issues no RPC and no compilation request, retains the
readings, and preserves frontend display preferences. The separate raw signed
`value` capture operation remains available for explicit diagnostic requests;
the default meter no longer uses it.

`MeterBallistics` runs only on the client, in linear amplitude before either
scale mapping. Initial tuning: 120 ms peak hold; downward acceleration relative
to the held peak, with upward snaps resetting velocity; RMS attack/release time
constants of 15/100 ms. Motion advances by elapsed time, not frame count, and
runs through `requestAnimationFrame` only while visible and unsettled. Source
activation changes and unavailable data reset old motion. Ballistics describe
received block measurements; they do not recover transient peaks from audio
blocks omitted by replaceable telemetry.

Added regressions cover collection cadence under the 20 ms runtime poll, missed-
deadline skipping, equal capture identities across mode changes, peak hold and
accelerating fall, momentum reset, frame-rate-independent motion and silence.
Owner verification of builds, tests, visual timing and CPU cost remains pending.
The agent has not run builds, tests or profiling.


### Accepted remaining-work order after meter verification, 2026-11-16

The owner reports all tests pass for the shared meter readings and smooth peak
motion, and accepts the following order after reviewing the backend-first
suggestions. Earlier dated priority lists remain as historical checkpoints.

1. Make configured node presentation structure available before audio-graph
   activation: identities, ports, connections and control descriptions come
   from configured graph data once the required package definitions are ready.
   Runtime observation availability remains separate. This is the active feature.
2. Implement Stage 5's bounded Tock observation query with a simple waveform.
   Exercise huge logical Regions, repeated navigation, revision consistency,
   cancellation and playback preparation priority. Repair concrete background
   execution gaps revealed by this use case; bounded computed-window allocation
   and rolling playback already have implementation and regression coverage.
3. Unify recording and Tock/buffered storage, including page reuse, snapshot
   lifetime, reclamation and resource limits. This is a separate complete
   feature, not an assumed prerequisite for computed-output observation.
4. Implement output gain and graph-authored snapping as separate complete
   features with their existing semantic requirements.

The requested amplitude-density and time-frequency views remain deferred.
Further presentation refinement remains recorded; this order prioritizes the
backend dependencies without discarding those requirements. Builds, test runs
and profiling remain owner-run.


### Package selection clarification during metadata work, 2026-11-16

The owner selects logical namespace ownership by **longest matching dotted
prefix**: separate packages may claim nested namespaces; duplicate exact
namespace claims are invalid. Logical namespaces remain distinct from source
paths, discovery provenance and build-cache identity. Static manifest package
dependencies determine the closure needed before configuring desired nodes.

The owner then revises the offline-catalog choice: build the current project
graph's package dependency closure first, and compile unrelated discovered
packages in a separate later pass. That later pass preserves discovery of all
public nodes through actual compiler-generated definitions, without requiring
a separately distributed offline catalog. It must not delay publication of the
current graph's configured structure or trigger GraphJit recompilation merely
because an unrelated definition became available. Package work remains outside
the audio callback.

Implementation is in progress; this records the chosen contract, not a claim
that namespace manifests or the two-pass selection are implemented.


### Configured-metadata publication friction, 2026-11-16

Source inspection during the active feature finds that
`ProjectGraph::rebuild_locked()` constructs `ProjectGraphGeneration::graph`
before invoking GraphJit, but installs `current_generation_` only after
compilation/staging. `handle_socket_rpc_get_graph_scope()` reads configured
interfaces while holding the same `ProjectGraph::mutex_`; therefore it cannot
expose a newly configured structure while that synchronous build holds the
lock. `ProjectGraphPublication` also returns through the completed rebuild
transaction. Merely removing an audio-activation check does not fix this.

The owner is being asked to choose between a separately identified pending
presentation snapshot while keeping synchronous edits, and committing
configuration before serialized DSP compilation. The latter would change the
synchronous compilation/publication contract in `startup_realization_order.md`.
No such architectural change has been made pending that decision.


### Accepted configured/executable publication boundary, 2026-11-16

The owner chooses to commit a valid immutable `ConfiguredGraph` synchronously
and publish its authoritative interfaces before DSP compilation. A serialized
ProjectGraph worker compiles pinned configuration/definition snapshots without
holding the project edit mutex. Obsolete queued requests are replaced; obsolete
completed requests cannot stage. Compiler failure leaves configuration committed
with a diagnostic and keeps the previous executable running. Executor preparation
is separate from the short guarded publication of an accepted chain link.
Already accepted links preserve the executor's ordered transition/migration chain.

Configuration and executable readiness are independent milestones. Observation
frames and live controls must name the realization they belong to; an older
executable's values must not be shown as measurements of newer configured ports.
Whole-project requests and manual-control requests use one serialized compiler
worker. The package-closure-first/discovery-later selection remains part of the
active metadata work, with longest-prefix namespace ownership. No offline catalog
is required. Superseded synchronous-publication documents are preserved in
`docs/historical/*_before_async_configured_publication_2026-11-16.md`.


### Configured metadata and serialized DSP source checkpoint, 2026-11-16

`ProjectGraph` now commits immutable configured structure before queuing DSP
work. One worker serializes whole-project and control compilations, pins the
exact inputs, coalesces obsolete queued work, and rejects obsolete completed
work before executor publication. Configuration failures retain diagnostics and
no valid successor; compilation failure retains the committed configuration and
previous audio. Storage-changing manual overrides use this same queue.

`GraphExecutor::prepare()` owns complete storage/migration resources off the
project edit lock. A move-only preparation handle publishes once or discards
off-thread; publication preserves the ordered accepted-generation chain. Initial
playback preparation is handed to the background worker without waiting under
the project lock. Application/benchmark shutdown joins package and compiler
workers before tearing down bridge bindings.

Configured-generation identity is distinct from executable target identity.
Observation-only replanning and control storage preparation can create a new
executable without inventing a new structural revision. `graph.getScope` exposes
execution phase/message and prepared/active configured-generation IDs. Ports and
control descriptions are configured data; measurements remain unavailable until
the correct executable supplies them. Observation delivery checks configuration
identity, and the frontend clears/rejects old frames when configuration changes.
`observations.reconcile` reports `schedulingUs`; the former synchronous
`compilationUs` field would misrepresent the new asynchronous work.

Manifests declare logical namespaces and exact package dependencies. Discovery
uses longest dotted-prefix ownership, validates duplicate claims, selects the
required dependency closure before catalog-only work, and rejects exports owned
by another discovered package. A missing dependency fails once and can recover
after discovery changes; it never causes a compile-everything fallback. Later
catalog publications retain unchanged configured/executable generations.

New regression sources cover blocked compilation with concurrent edits, stale
result rejection, discarded preparation handles, old observation frames,
namespace priority/ownership, missing-dependency recovery and duplicate claims.
The edit benchmark separately records configuration commit (`configured_ms`)
and end-to-end executable preparation (`wall_ms`), so asynchronous return cannot
artificially improve the original latency result.

Owner verification is pending. No builds, tests, typechecks or profiling were
run by the agent. Rebuild both server/tests and client, run the backend and client
suites, then inspect startup/graph edits with audio running. Later Stage 5 bounded
Tock observation is still the next complete feature after this checkpoint.

The package worker also receives completion for discarded compiler requests, so
a newer configuration failure cannot leave catalog discovery waiting forever
on an obsolete executable. Namespace metadata is included in package catalog
records for classifying unresolved nodes before providers load.


### Owner verification and eight-failure follow-up, 2026-11-16

The owner rebuilt and ran 1,078 tests: 1,070 passed and eight failed. The agent
did not run builds/tests. Follow-up source changes:

- Retain rootless required-definition IDs for static namespace selection; the
  unresolved-instance test now verifies that the emitted path hint stays empty.
- Wait explicitly for compilation when a bridge test asserts executable status;
  a no-op control test compares configuration identity/graph rather than the
  enclosing snapshot pointer, which can change on worker status publication.
- Restore the `prepared` control status for storage-changing compilation before
  staging; this preserves the release/settlement progress contract.
- Mark both packages required in the atomic mixed-result fixture. Exercise
  defensive duplicate-ID handling directly at PackageDefinitions, since the
  production watcher must reject invalid ownership before that layer.
- Keep logical test package requirements rootless rather than retaining a second
  declaration keyed by a filesystem root in the first-build recovery helper.
- Explicitly discard the async test's intentionally unused created-node ID to
  remove the `nodiscard` warning.

The owner should rerun builds and tests to verify these corrections.


### Owner verification: asynchronous snapshot test corrections, 2026-11-16

The next owner run passes 1,073 of 1,078 tests. Four failures compare enclosing
`ProjectGraphGeneration` shared pointers while the compiler can publish a newer
execution-status snapshot of the same configuration. Tests for failed edits and
replacement previews now assert configured identity, executable-target identity,
configured graph and pinned definitions. The remaining equivalent assertions
were corrected together to avoid the same timing-dependent failure on a later
run. The catalog-only publication regression retains exact snapshot equality
after waiting for compilation, because that case deliberately publishes nothing.
The observation-revision test now waits for queued compiler work before inspecting
the request's captured requirements. No production behavior is changed by this
follow-up; owner-run verification remains pending.

### Startup follow-up: builtin import and failed-root meter structure (2026-11-16)

The owner's startup run exposed a missing implicit package dependency: module
input defaults construct builtin constants even when package source has no
literal builtin-node lookup. `iv.builtin` is now included automatically in
package selection and accepted by registered-definition dependency validation.
Other cross-package constructions still require declared dependencies.

`NodeInstanceFailureInterfaces` now keys its retained successful configuration
owners by instance ID. Observation capability discovery uses those current desired
closed graphs when another instance or a root connection prevents the root from
configuring. It can advertise meter structure with no executable or committed
root generation; readings remain unavailable. It never borrows old executable
ports for an invalid desired instance. NodeRef-dependent results without a closed
graph still expose their existing port metadata but do not gain observation
capability discovery through this path.

Dotted connections indicate saved desired connections that have not been applied
to a successfully configured root. A public definition ID equal to its package
namespace is valid, along with dotted descendants subject to longest-prefix
ownership. Regression tests cover automatic builtin closure selection, registered
builtin construction, and desired healthy meter capabilities during a failed root
configuration. Owner-run build and tests are pending; the agent has not run them.

### Stage 5 active feature: bounded output windows (2026-11-16)

The owner reports all 1,081 tests pass and the application functions after the
configured-metadata checkpoint. Builds, tests, typechecks and profiling remain
owner-run.

Next complete feature: representative Sample waveform windows for Tock outputs
and published buffered outputs, from background execution through the generic
Vue lane. These are bounded, approximate position samples, not exact peak
envelopes. Requested positions, exact sparse coverage, unavailable contents,
configuration identity and persisted semantic/data versions remain distinct.
Projection must copy its bounded result before transaction storage expires.

Implementation order within this feature:

1. Register the window operation and connect projection to the existing
   background transaction's selected output views.
2. Queue bounded, cancelable window work on GraphExecutor's BackgroundExecutor,
   after playback/cutover work; never read mutable Tock state on a control thread.
3. Extend Observations' batched scope protocol and version checks without
   introducing live capture probes or DSP recompilation for viewport changes.
4. Connect the lane timeline/visibility to window requests and render truthful
   gaps. Retain live meters for Sequential outputs and reuse the generic host.
5. Add regression coverage for sparse projection, lifetime/version boundaries,
   stale requests and the complete frontend request/result path. Hand verification
   to the owner before taking the next feature.

This records active work, not completion. Exact envelopes, multiresolution caches,
Tock State and input observations remain deferred. Complete buffered-output
publication remains the existing runtime's responsibility, not a viewport policy.


### Stage 5 representative-window source checkpoint (2026-11-16)

Implemented in source; owner verification is pending:

- Sample's type-owned operation catalog includes background-only `waveform`.
  Capability discovery separates window operations from live operations, retains
  public channel names and advertises configured structure before execution.
- Reconciled interests optionally name an integer project-sample window and a
  representative point count. Compatible windows share work across scopes and
  clients. Replacing a viewport, losing a scope or changing configuration cancels
  obsolete work without interrupting an already running background transaction.
- `GraphExecutor::BackgroundExecutor` services these requests only after its
  playback/cutover work and cold commands. Its generation owns Tock State.
  Projection runs after evaluation validation and before temporary storage expires;
  delivery receives copied scalar values, not borrowed views or generation storage.
- Results carry configuration/execution identity, semantic/data versions, native
  pace, actual logical Region fragments and explicit unavailable values. Overlaps
  remain separate. Covered zero, missing coverage and unavailable contents differ.
  Buffered Tick output queries read recorded samples; they do not replay Tick.
- Initial tuning limits are 512 representative positions, 2,048 result values or
  Region fragments, and 16 MiB of newly selected sample materialization per query
  transaction, including discovery evaluations. These are bounded query limits,
  not a guarantee that arbitrary authored callbacks are cheap. Exceeding the sample
  budget reports a failed view request and leaves playback policy unchanged.
- Published-data changes refresh a compatible window at most four times per second.
  The preceding immutable result can remain visible while explicitly marked
  outdated/updating; it retains its own provenance. Source/version replacement
  clears incompatible data. Invalidation currently uses the conservative global
  persisted-data revision; per-output/intersecting-range reuse remains an optimization.
- The generic lane renders background Sample waveforms against the shared timeline.
  Automation's `mini` mode uses the same actual Tock window path with its existing
  vertical bounds; `nano` shows coverage intervals, while its editable SVG and
  point gestures remain frontend-owned. Nano requests reduce point count to two.
  Generic Sample waveform scale is fixed at −1..1. Status-bar hover information
  identifies representative sampling, pending work, older snapshots and errors.

New regressions exercise gap/overlap/unavailability projection, signed and bounded
RPC windows, shared/canceled requests without live-probe compilation, data-version
refresh, real computed-Tock projection/lifetime, recorded Tick reads, paced rendering,
and viewport/configuration/data-version rejection in the client.

The agent ran only source inspection and `git diff --check`. The owner should run
the usual backend build/CTest, client tests and typecheck, then reinstall the client.
For a direct UI check, zoom an automation lane down to `mini`, pan/zoom horizontally,
and zoom down further to `nano` to inspect its coverage. Editable mode retains the
point editor. Inspect a generic buffered Sample output similarly when available.
Exact peak envelopes and derived multiresolution caches remain deferred.


### Stage 5 owner verification: three-failure correction (2026-11-16)

The owner ran 1,087 tests; 1,084 passed. Two failures exposed runtime selection
bugs and two assertions had fixture mistakes:

- `finalize_tick_runtime_plan` included authored Tock Region/Coverage inputs among
  complete-buffer Tick readers. Initial playback preparation consequently
  requested full-circle materialization of a dense replay dependency. Tock nodes
  now keep their background bindings and have no Tick invocation bindings;
  validation rejects a Tock random-access binding in the Tick plan. Existing
  topology and real waveform regressions assert this domain boundary.
- Recording subwindow queries selected their clipped sample positions as Region
  descriptors. `PersistedSampleReader` requires the original published Region
  identities and could not match those descriptors. Transaction storage selection
  now retains intersecting published recording Regions while requested coverage
  stays bounded. The real recording waveform regression checks captured values.
- The window RPC fixture now supplies the required empty node-address `path`.
  The recording assertion uses the test helper's begin/end convention for [4,8).

The agent inspected the owner log and source and ran `git diff --check`, without
running builds, tests or profiling. Owner verification of these fixes is pending.


### Stage 5 application follow-up: observed replay and automation presentation (2026-11-16)

The owner reports 1,087 passing tests, but the application failed compiling an
observed simple-sine graph with `GraphJit sequential input has no imported neutral
initializer`. `observation_sink_operations` had a host helper for initialization,
but its `NodeCompilerOperations`, runtime `NodeImplementation`, and compiler-owned
`PrimitiveCallbackPlan` import path omitted the executable initialization callback.
Those records now carry the same type-correct initializer through
`CallbackImportAbi::sequential_initialize`. It accepts complete Sample storage
spans, including empty spans, just like authored initialization callbacks. A new
compiler/runtime regression attaches an observation sink to a Tock source,
forcing background delivery. It exercises the generated neutral initializer,
activates the graph and checks the captured samples.

The owner also rejected the generic output display appearing on the automation
module. This supersedes the Stage 5 mini/nano integration described above:
`AutomationCurvePresentation` keeps its dedicated SVG in every display mode.
Mini/compact draw the read-only authored curve, nano draws its covered intervals,
and editable draws points and accepts point/segment mutations. No generic meter
or output-observation subscription is installed by that specialized presentation.
Generic presentations retain the new background waveform implementation.

The agent inspected source and the application log and ran `git diff --check`.
Builds, tests, typechecking and profiling remain owner-run; verification is pending.


### Earlier editing threshold (owner refinement, 2026-11-16)

The owner wants editable lanes at roughly the former mini height, mini at the
former nano height, and a shorter nano mode. Frontend tuning now uses a 96 px
editable threshold (previously 200 px), a 32 px nano/mini threshold (previously
64 px), 6 px height hysteresis (previously 12 px), and a 24 px minimum lane
height (previously 32 px). These are presentation tuning values, not backend
contracts. Saved lane geometry is retained and selects its mode using the new
thresholds. Existing geometry assertions are updated; owner-run client tests
and typecheck remain pending. No build or tests were run by the agent.


### Dense lane zoom and independent header layout (owner refinement, 2026-11-16)

Further owner tuning sets editable to 48 px, nano through 16 px, and the minimum
lane height to 12 px. Mini occupies the interval between nano and editable, with
32 px as its representative height. Height-based mode selection has no hysteresis:
the same allocated height yields the same content mode in either zoom direction.
The existing compact-width hysteresis is separate and remains unchanged.

`NodeLane` now uses its own size container for header layout. At 72 px or less,
the header moves its port groups alongside the name, before the stacked header
would lose its handles. This responds to header space independently of the
presentation's content mode. The name's font is the smaller of the base font and
lane height minus 4 px. Nested port slots retain their dimensions, hit areas,
focus behavior and popovers while their normal border is transparent.

Lane content and its allocated presentation host no longer create internal
scrollbars. Sample meter groups/channels can shrink below their previous 20/9 px
minimum heights. Short lanes reduce label detail and channel spacing while
retaining both stereo meters; the surrounding timeline owns vertical scrolling.
The existing mode regression now checks direction-independent selection across
repeated zoom-in/zoom-out sequences. Source inspection and `git diff --check` only;
client tests, typecheck, extension rebuild and manual UI checks remain owner-run.


### Meter hover ownership and dense geometry follow-up (2026-11-16)

The owner adjusted the independent inline-header breakpoint from 72 to 56 px and
removed stereo channel labels beside the meters. Both adjustments are retained;
channel names remain available in hover status and accessibility labels.

`PresentationObservations.hint()` now preserves interaction order when telemetry
updates a hint, with pointer hover taking priority over keyboard focus.
`LiveOutputMeters` watches the semantic address key, so replacing a graph's JSON
objects does not remove and re-register every unchanged node's interest.

Configured-generation invalidation still clears current readings and rejects old
frames. While awaiting a replacement frame, `SampleMeter` can retain a frozen,
dimmed previous drawing. Hover status and accessibility text identify it as a
previous reading; it is not exposed as a current numeric accessibility value.
An explicit unavailable result clears it. Fresh activation resets motion, and
removing a source removes its component and retained drawing. This follows the
existing observation continuity contract without accepting old-generation frames.
The in-meter out-of-range text is removed; exact values remain in hover status.

Lane content padding now varies continuously with height instead of switching
at 40 px. Automation uses fractional measured SVG dimensions and fills its
viewport without aspect-ratio letterboxing. Folded port circles use vertical
centering, matching direct port circles at the smallest lane height.

[Vue visual tuning](./vue_ui_tuning.md) maps common adjustments to their source
files and provides an extension-only rebuild command. A new client regression
covers status ownership with interleaved channel updates and hover/focus changes.
Only source inspection and `git diff --check` were performed by the agent;
client tests, typecheck and manual UI checks remain owner-run.


### Compatible meter continuity across automation edits (2026-11-16)

The preceding hover/dense-geometry follow-up did not resolve the owner's meter
flicker report. Its frozen, dimmed pending drawing and unconditional activation
reset are superseded by this change. The lane and `SampleMeter` components were
already kept alive by Vue's address/channel keys; the remaining invalidation was
in telemetry delivery and animation state.

`Observations::collect()` previously emitted `available: false` whenever its
configured target was newer than its most recent executable frame. It now marks
that case `pending: true`, so compilation in progress is distinguishable from
unavailability after cutover. A missing/failed configuration does not claim to
have an executable pending. Current-generation and activation-order checks remain
in place; previous frames are not accepted as new measurements.

`PresentationObservations` keeps capture identities for compatible semantic
sources, operations, selected channels and result schemas. An incompatible
channel/schema gets a new identity; `LiveOutputMeters` also keys the meter's
component by node definition. Unchanged neighbouring channels keep their own
identities. There is no graph-generation component key.

`MeterPresentationState` owns copied scalar history and `MeterBallistics` across
compatible activations. Missing/pending frames retain the picture and motion
without opacity changes. Status/accessibility identifies previous measurements;
explicit source unavailability, removal, replacement or hiding clears history.
`SampleMeter` uses this state directly, rather than resetting on every activation.

Regressions cover the server's pending/invalidated distinction, continuous peak
and RMS motion through generation changes with late-frame rejection, and stable
versus incompatible channel/schema identities. Source inspection and
`git diff --check` only; the backend rebuild, C++/client tests, typecheck and manual
curve-drag verification remain owner-run.


### Visualization context menu and nano curve shape (2026-11-16)

The owner removed the need for an output-name column and scale-switch button
beside live meters. `LiveOutputMeters` now gives that space to the visualization
and opens a context menu on right-click. Level (dBFS) and Value (linear) remain
the two current choices, using the existing saved per-output preference. Both
stereo channels share that preference; selecting a mode retains their component
identities, readings and motion and does not change capture requirements.
Output/channel names remain in hover status and accessibility labels.

The menu marks the current choice, fits its position to the webview, supports
the context-menu key/Shift+F10, arrow/Home/End navigation and Escape, and dismisses
on outside interaction, scrolling, resizing or window blur. Selecting a choice
or pressing Escape returns focus to the meter.

`AutomationCurvePresentation` now renders its existing sparse curve path in
every display mode. Nano no longer substitutes coverage rectangles; it scales
the same curve to the smaller lane height, with empty segments remaining gaps.
Point and segment editing remains restricted to editable mode. The superseded
presence-only direction is preserved under `docs/historical/` and linked from
the updated normative presentation document.

These are frontend presentation changes. Source inspection and
`git diff --check` only; extension rebuild, client tests/typecheck and manual
menu/zoom verification remain owner-run.


### Latest peak versus momentum peak; visualization preferences deferred (2026-11-16)

The owner clarified that the meter should compare the latest received peak with
the held/momentum peak, rather than showing an RMS fill beside a peak marker.
`MeterPresentationState.reading()` now keeps the live peak position separate from
`heldPosition`; `SampleMeter` uses the former for `.peak-fill` and the latter for
the held marker. The fill follows incoming peaks immediately in both directions.
`MeterBallistics` animates only the held peak, retaining its hold and accelerating
fall and the existing compatible-transition continuity. Unused RMS animation is
removed. Actual RMS measurements remain in the numeric readout. Each indicator
has its own above-reference colour state.

The default meter's `NodeLane` content bypasses the shared vertical padding, and
`LiveOutputMeters` no longer adds horizontal padding. The matching negative
waveform margin is removed so background visualizations also use their allocated
width without overflow. Channel separation remains unchanged.

Future visualization-specific refresh rates and display/performance settings are
recorded in the normative presentation document. They belong in VS Code with
presentation preferences for now, with telemetry cadence distinct from display
frame rate. No settings UI, configurable rate or backend capture change is part
of this work.

Regression sources now distinguish immediate peak changes from held-marker
motion, including independent reference warnings and compatible graph cutovers.
Source inspection and `git diff --check` only; client tests, typecheck, extension
rebuild and visual checks remain owner-run.


### Themed native scrollbar and selection overview (2026-11-16)

The owner requested reuse of VS Code scrollbars and an editor-like selection
marker if straightforward. The supported webview API provides an independent
HTML view and theme colour variables, rather than a reusable editor scrollbar
control. The timeline keeps its browser-native scrolling and styles the track
and normal/hover/active thumb with VS Code's `scrollbar`/`scrollbarSlider`
colours. References: [webview API](https://code.visualstudio.com/api/extension-guides/webview)
and [scrollbar theme colours](https://code.visualstudio.com/api/references/theme-color#scrollbar-control).

`TimelineScopePresentation` paints the selected lane's proportional range as a
thin marker in the native vertical track, using the overview-ruler selection
colour. It remains visible when that lane scrolls offscreen and follows lane
order/height changes. A minimum marker height keeps short lanes visible in long
graphs. No extra scrollbar widget, pointer handler, dependency or scrolling
implementation is added. Track clicks, thumb dragging and wheel input stay native.
The shared header-height value keeps marker geometry and zoom anchoring aligned.

Source inspection and `git diff --check` only. Extension rebuild and manual checks
of track markers, thumb hover/drag and scroll/zoom remain owner-run.

### Owner follow-up: input-wide connection targets (2026-11-16)

Recorded for later implementation: while dragging a connection, every element
belonging to a destination input should resolve that same input and preview the
ghost connection. Dropping on its knob, name, port or associated control area
should connect under the usual compatibility rules. This applies to direct
controls and popover/continuation entries; it must not require aiming at the
circle. Ordinary knob gestures remain unchanged. The normative rule is in
[input-wide connection drop targets](./vue_presentation_refinement_direction.md#input-wide-connection-drop-targets-owner-follow-up-deferred).


### One-sided knobs and declaration-owned scrub speed (2026-11-16)

Before starting the next larger feature, the owner reported a minimum-only
radial-range error and value-dependent drag sensitivity. `knobFraction()` now
uses distinct bounded, minimum-only, maximum-only and unbounded mappings, with
the finite bound at the appropriate arc endpoint. `knobScrubSpeed()` derives a
stable default from the declared range/default and accepts `scrubSpeed`; it
does not read the current manual value. Pointer drags and keyboard steps share
that declaration-owned policy. Ctrl-click initializes the existing power-of-two
sensitivity modifier to ×0.25; wheel changes and subsequent ordinary ×1 drags
retain their usual behaviour.

`SampleContinuousValueExtension` adds optional `scrub_speed` to Sequential and
boundary input options, sequential metadata and introspection metadata. The
hint survives boundary-to-sequential conversion and configured-graph binary
serialization, and is projected as `scrubSpeed` through the existing value-type
metadata. `WorkspaceSession` and `useManualInput` preserve it for the Vue knob.
No new RPC route or audio-thread work is added.

The reported zero-frequency/detune behaviour was traced to the saved
`simple_sine` graph: a nonzero automation curve is connected to detune. Clicking
temporarily overrides it with the stored zero; leaving restores the connection.
The owner confirmed this is the intended behaviour. No oscillator/GraphJit fix
is needed for that report.

Regression sources cover one-sided arcs, stable and explicit speeds, Ctrl/wheel
interaction, metadata JSON projection, and root/subgraph metadata archive round
trips. Source inspection and `git diff --check` only; backend/PCH/package/client
rebuilds, tests and typecheck remain owner-run.

The larger accepted remaining-work order still follows the verified metadata and
bounded Tock observations with unified recording/Tock buffered storage, then
output gain and graph-authored snapping as separate complete features. The
input-wide drop-target request remains recorded for later implementation.

### Meter hint re-entry ordering correction (2026-11-16)

The owner's client test run exposed that an already registered focused hint
did not renew its ordering when it became hovered. `PresentationObservations`
now updates ordering on priority transitions, while telemetry updates at an
unchanged priority preserve interaction order. The existing regression covers
focus-to-hover re-entry and now repeats interleaved same-priority updates.
`git diff --check` only; client tests remain owner-run.

### Next feature authorized: shared buffered storage (2026-11-16)

The owner reports all tests pass after the knob and hint-order corrections and
requests the next feature. The accepted order now advances to unified recording
and Tock/buffered storage. Scope, implementation order and verification obligations
are recorded before source changes in
[Shared buffered-output storage implementation](./shared_buffered_storage_implementation.md).
Further UI refinements, output gain and graph-authored snapping retain their
recorded positions rather than entering this storage feature incidentally.

### Shared buffered storage source checkpoint (2026-11-17)

Recording and Tock/buffered sample publication now share page allocation,
immutable page/view ownership, retired-offset reuse and resource accounting.
Unused cached sample backing can be reclaimed under pressure; valid buffered
contents remain authoritative. Event chunks share the retained-payload budget.
Tick materialization prepares residency before publication, and background work
reclaims retired snapshots before testing the next transaction's limits.

The [implementation checkpoint and owner-run measurements](./shared_buffered_storage_implementation.md#implementation-checkpoint-2026-11-17)
record exact source behaviour, provisional limits and remaining measurement
questions. Regression sources and `scripts/benchmark_buffered_storage.sh` are
ready. Only source inspection and static diff checks have been performed; owner
builds, tests and measurements remain pending before advancing the feature order.

### Shared buffered storage verification and measured friction (2026-11-17)

The owner reports all tests pass and ran all eight storage benchmark cases.
The retained backend log shows 1,089 tests with zero failures. The agent reviewed
the generated reports without rerunning builds, tests or measurements.

The [measurement and friction report](./shared_buffered_storage_implementation.md#owner-verification-and-measured-friction-2026-11-17)
records bounded recording reuse, dense-write mapping fragmentation and Tock's
extra backing for partial-window replacements. Shared storage is functionally
verified for the tests run; its measured representation costs remain open.
The proposed continuation is run-aware page reuse, followed by per-logical-Region
Tock backing and patch measurements, before proceeding to output gain and
graph-authored snapping. This recommendation awaits discussion; it does not
silently replace the accepted storage or retention contracts.

### Approved storage fixes implemented (2026-11-17)

The owner approves both measured fixes. Ordered free-page reuse and canonical
page backing per logical Tock Region are implemented, with immutable snapshots,
sparse validity and independent duplicate Regions preserved. Prepared planar
views can map shared runs directly or use bounded repacking when required.
Regressions and the expanded storage benchmark cover the measured costs and
their ownership boundaries. See the [approved follow-up checkpoint](./shared_buffered_storage_implementation.md#approved-follow-up-implementation-2026-11-17).
Owner rebuilds, backend tests and fresh measurements remain pending before this
storage follow-up is consolidated or the next feature begins.

### Storage measurements reviewed; focused verification pending (2026-11-17)

The owner rebuilt, reports the suite passes and supplied fresh storage results.
All 80 benchmark rows finish with live backing equal to their payload size.
The observed 4 MiB dense recording median with 1 MiB views falls from 6.073 ms
to 2.239 ms; dense Tock falls from 5.390 ms to 2.092 ms. Recording mapping calls
fall from 99,477 to 800 across 100 dense writes, and scattered Tock updates no
longer retain the earlier redundant pages.

Review also exposed the agent's test-registration mistake: the 19
`SharedBufferedStorage` cases were compiled but added after configure-time test
discovery, so they were absent from the passing 1,089-case CTest run. CMake now
includes the source before discovery through `add_iv_gtest(EXTRA_SOURCES ...)`.
The [measurement and correction checkpoint](./shared_buffered_storage_implementation.md#follow-up-measurements-and-test-registration-correction-2026-11-17)
records the focused owner command. No benchmark rerun is needed for this
registration correction. Run those cases before declaring the follow-up complete
and moving to output gain. Builds, tests and measurements remain owner-run.

### Shared storage verified; output gain authorized (2026-11-17)

The owner reports all 1,108 tests pass, including the newly registered storage
regressions, and requests the next feature. Shared storage and its measured
follow-up are consolidated for the tested scope; further tuning remains driven
by real workloads. Output gain is now active, ahead of graph-authored snapping.

The owner clarifies that gain is a non-destructive output transform, including
retroactive reads of buffered contents. The short-lived same-session proposal
to restrict gain to Sequential/computed outputs was withdrawn before source
implementation. The [output gain implementation record](./output_gain_implementation.md)
retains the accepted contract, implementation order and concrete source gaps.

### Tick coverage intent recovery before output gain (2026-11-17)

The owner identified the November 16 full-only Tick coverage migration as a
semantic regression. The [recovered contract](./tick_input_union_and_feedback_direction.md)
restores acyclic `input_union` coverage and uses effective full live coverage
only for feedback SCC members, which cannot be contextually replayed. Restore
that prerequisite before finishing output gain with multiplication nodes.
The owner also requests removal of unnecessary unstaged gain-specific code.
Output gain remains unfinished and unverified; no build/test/profile has been
run by the agent. Preserve the existing computed/buffered and windowed-storage
work. Do not confuse the earlier passing suite with verification of recovered
intent.

### Buffered reconstruction and cache priority follow-up (2026-11-17)

Owner decision: Sequential/buffered recordings are non-recomputable;
Region/Coverage buffered results are recomputable for the selected snapshot,
with cache priority over computed results. The normative contract is in
[output capabilities](./output_capabilities_and_background_demand.md#reconstruction-and-cache-priority-owner-decision-2026-11-17).
This supersedes the earlier preserve-all-valid-Tock-payload policy. Implement
snapshot-correct, pin-safe eviction/reconstruction and relative cache priority
soon after the active Tick coverage restoration and output-gain cleanup; the
shared-storage implementation record carries its verification requirements.
No differentiated eviction implementation or measurement is claimed here.

### Status bar hints

The graph editor and source inspection view share one VS Code status item
for hints. The item follows pointer hover, keyboard focus, and active
knob, automation point, or ruler drags. Build and compilation details
appear in their own status items and in the output channel. Source checks
cover the tooltip migration. Build and UI checks remain owner-run.

### First pass on knob and port visuals

`KnobFace.vue` draws the default pin and separate markers for each finite
bound on the same arc. The exact default gets 24 pixels of extra drag travel.
Four bars show logarithmic distance from the default on an unbounded side.
A larger face and a value appear above the knob after a short hover or when
a drag starts. The lane uses the status bar for its numeric readout.

`NodePortRow.vue` shares the knob, circle, and label layout between direct
and folded rows. The last visible row anchors the scrollable continuation.
The input row accepts compatible cable drops and draws the preview to its
port circle. Wires between visible unselected ports are faint. Labels and
knob faces cover the wires beneath them.

This pass tries orange and purple for Event inputs and outputs, with a
distinct port shape and dotted cables. The palette, magnitude bars, and
temporary preview are open for owner feedback.

The wire mask now follows the width of the label text. Wires remain visible
over the rest of the label background. Knobs and ports share a static
outline with two distinct controls. At minimum lane height, the compact
handle fits inside its lane and opens the full-size folded rows.

After owner feedback, the socket sits partly over the knob arc. Its hit area
extends toward the cable and leaves the center of the knob open for dragging.

The next pass puts the dial face in front of the socket. A separate grab
area extends toward the cable from the exposed part of the socket.

A circular cutout now leaves a gap between the socket and the dial arc.

The owner chose the compact cutout style after the
[A/B comparison](./historical/vue_knob_style_comparison.md). The timeline
no longer shows the style toggle. `KnobFace.vue` shades the area between
the default marker and the current pointer on the dial and in its preview.

`GraphEditor.vue` now keeps unselected cables faint through label areas.
Selected cables fade to the same faint level near the visible text and
brighten away from it. The dial boxes grow to 24 pixels in compact rows and
31 pixels in taller rows when space allows. At minimum lane height, ports
still use the folded slot.

`NodePortRow.vue` centers three dim dots beneath the last visible knob on
each side with hidden ports. If the row has no knob, the dots sit under its
port. The compact header makes room for the 24-pixel knob and its 8-pixel
indicator. If the row and dots do not fit, the folded slot shows a single
port circle or a stack of two.

The overflow dots now sit in a horizontal row at the bottom of the header.
The last visible knob or port sets their horizontal position.
The folded rows start one row gap below the last visible control, even when
the dots sit farther down.
The dots use solid 2-pixel marks. Port popovers pass modified wheel events
to the timeline. Plain wheel input scrolls the popover.

Output names sit to the left of their port circles. Folded output rows
keep the same right edge as the direct rows, so longer names can extend
left while the controls line up. The polarity button beside the gain knob
shows `+` or `−` and preserves the gain magnitude when toggled.

The polarity button sits close to the dial. The normal sign is dim and the
inverted sign has a blue background. After zoom changes the port layout,
the editor checks the pointer again and opens the continuation if the
hovered side has hidden ports.

The continuation starts at the next row position with no extra top padding.
Input knob labels sit near the upper right of the dial. Output knob labels
sit near the upper left.

At mini heights, both single and grouped ports fold into port symbols. The
standalone popover shows full-size knobs and wraps long port names. It can
shift inside the viewport to retain room for the names near an edge.

`NodeLane.vue`, `LiveOutputMeters.vue`, and automation editing use the
same `editable` to `mini` transition. Knob rows stay beneath the lane name
throughout `editable`. The stereo channel height no longer sets an earlier
cutoff for meter scale labels.

On zoom, `NodePorts.vue` reads the current port area height and waits for the
resized rows before placing a hover popover. An open continuation also
realigns when the knob size changes and the same ports stay visible.

Lane names use a softer theme colour. Port labels use a quieter base colour
with a small tint from their port. High-contrast themes use the theme
foreground for both.

`TimelineScopePresentation.vue` supports Alt + middle-button drag for
vertical zoom and Ctrl + middle-button drag for horizontal zoom. Alt uses
horizontal movement and anchors at the cursor's current height. Ctrl uses
vertical movement and anchors at the cursor's current horizontal position.
Port popovers forward the same gestures to the timeline. Each pixel of drag
applies twice the zoom of a pixel of wheel input.
