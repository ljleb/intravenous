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

Stage 2, started 2026-11-12. Stage 1 is complete.

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

#### Known scope-port defect

`ProjectGraph::handle_socket_rpc_get_graph_scope` fills ports only from
`current_generation_->placements`. If one invalid instance prevents the first
successful whole-project generation, the scope has no ports even for a valid
definition such as `iv.project.saw`. The editor must distinguish availability
of a configured node interface from whole-project execution success. Do not
pretend that changing the timeline presentation fixes this backend defect, or
relax whole-project execution atomicity merely to expose UI metadata.

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

**Proposed next design discussion:** define the playback-position owner and
play/pause/seek behavior before exposing a moving playhead. Seconds and project
sample indices can display the same position using the project sample rate;
they do not require separate clocks. Beats remain undecided. An explicit node
that produces periodic beat events does not require a built-in tempo map.
No playback API, control gesture, or tempo policy is introduced by this pass.
After this bounded discussion, resume Stage 2's remaining scope/port and editing
work rather than treating ruler navigation as a replacement for those steps.

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
