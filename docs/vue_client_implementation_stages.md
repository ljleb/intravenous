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

Stage 1, started 2026-11-12.

### Stage 1 implementation status (2026-11-12, written, not yet built or tested)

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

Remaining before the stage 1 completion criterion can be checked: run `npm install`
in `vscode/client` (this updates `package-lock.json`, which `install_client.sh`
needs for `npm ci`), build, typecheck, run the client and C++ tests, and open an
existing project, including a window reload and a server restart.
