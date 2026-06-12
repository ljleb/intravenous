# Lanes Visualization Direction

This note captures the current intended direction for lane visualization and UI
refresh behavior.

It should be read together with
[task_runner_execution_direction.md](./task_runner_execution_direction.md).
That document covers execution scheduling. This document covers how lane data
should be prepared and delivered for UI display.

It should also be read with the current intended split between:

- `Timeline` as the structural lane-graph app module
- a separate execution app module, tentatively `TimelineExecution`, which owns
  the executable view derived from `Timeline`

## High-level model

The timeline/lane graph is intended to be the single unified graph for both:

- compiled timeline content
- realtime/live content such as microphones or controls

Everything is represented as lanes.

Visualization should stay inside that same model rather than introducing a
separate observer graph or unrelated sampling pipeline.

However, the executable state derived from that graph should not live directly
inside `Timeline`. The current intended shape is:

- `Timeline` owns the editable structural lane graph
- `TimelineExecution` derives lane execution bindings, runtime port memory, and
  task-runner registrations from `Timeline`
- `LanesVisualization` builds on top of those execution results

## Multiple lane views

There are multiple lane views, not one global view.

These views are already modeled through `LaneFilters` and `LaneViews`, and are
intended to be displayed through multiple VS Code webviews.

Different lane views may:

- show different lane subsets
- use different visible windows
- use different display resolutions

Visualization behavior must therefore be view-aware and support multiple active
views at once.

## Visualization app module

A new app module, `LanesVisualization`, should own visualization-specific graph
maintenance and UI refresh coordination.

Its responsibilities should include:

- tracking which lanes need visualization based on active lane views
- maintaining builtin visualization lanes for visible lanes
- reconfiguring those visualization lanes when lane views move or resize
- publishing batched visualization notifications to the UI on a UI-oriented
  refresh cadence

`LaneViews` should remain the owner of lane-view state. `LanesVisualization`
should collaborate with it rather than replacing it.

`LanesVisualization` should also collaborate with the execution module rather
than treating `Timeline` as an execution engine. `Timeline` is the source of
graph structure. The execution module is the source of current executable lane
outputs.

## Visualization lanes

Visualization should be represented by builtin system-managed lanes connected to
the outputs of lanes that need display data.

This means `LanesVisualization` should:

- create visualization lanes for visible lanes
- remove them when no longer needed
- reconfigure them when view parameters change

The visualization lane node type should be specifically designed to:

- receive lane output samples or events
- compress or reduce them for display
- sample windows sparsely when needed
- drop or aggregate blocks when needed
- target a specific refresh rate

Scrolling or resizing a view should reconfigure these visualization lanes.

## Sampling intent

Lanes need to support two different kinds of read behavior:

### Sparse sampling

Compiled lanes should support sparse window sampling for UI display.

This means sampling a window by:

- start
- end
- requested number of evenly distributed points

This is needed for:

- curve drawing
- event display
- timeline previews

### Causal sampling

Both compiled and realtime lanes also need causal block-by-block sampling for
playback.

This means:

- consecutive blocks
- in order
- one block at a time

Realtime lanes naturally participate in causal playback. Compiled lanes may also
be consumed that way during playback.

Execution and sampling should therefore be separated:

- execution writes lane-owned outputs
- sampling reads those outputs in sparse or causal form

That execution state is intended to belong to `TimelineExecution`, not to the
structural `Timeline` module directly.

## UI refresh cadence

UI refresh must not be tied directly to audio block completion frequency.

Even if lane execution runs faster than UI refresh, the UI should receive
updates at a stable visualization cadence, for example around 30 FPS.

For now, `LanesVisualization` should own that notification cadence and publish
batched notifications to the UI.

This means:

- lane execution may run at audio/task-runner cadence
- visualization lanes publish their latest visualization-ready state as it
  becomes available
- `LanesVisualization` wakes up on a UI-oriented cadence
- it gathers the latest visualization data for active views
- it sends one batched notification

This is preferred over notifying the UI once per finished audio block.

## Realtime versus compiled visualization refresh

### Realtime lanes

Realtime lanes should usually contribute fresh data continuously as execution
runs.

Most UI visualization ticks will therefore mostly reflect realtime lane updates.

### Compiled lanes

Compiled lanes usually do not need repeated refresh just because audio blocks
continue.

However, when lane views move or resize, compiled lanes should be re-sampled so
they continue to display correctly for the new visible window or display
resolution.

The intended control flow is:

- `LaneViews` detects view changes that require compiled visualization refresh
- `LaneViews` notifies `LanesVisualization`
- `LanesVisualization` manages the corresponding resample/reconfigure work
- the resulting compiled visualization data is folded into the next batched UI
  visualization notification

Compiled lanes should therefore refresh on demand from view changes, not on
every audio block.

## Lock-free publication

Visualization data published by visualization lanes should be made available to
`LanesVisualization` without any locking or blocking on the audio/execution
side.

The preferred direction is a simple atomic publication strategy:

- each visualization lane owns a small visualization output state
- writes happen into a non-current slot
- once a full visualization payload is ready, the lane atomically publishes the
  new slot/version
- `LanesVisualization` reads the latest published slot on its own refresh tick

This is effectively a small per-lane lock-free snapshot mechanism, such as
double buffering with atomic publication.

The execution side must not:

- wait for UI consumers
- lock on behalf of UI readers
- block while visualization is being collected

## Relationship to task-runner execution

Lane execution is not intended to remain pull-based.

With the task runner, execution should proceed forward in dependency order using
independently executable lane units.

Visualization should fit into that execution model:

- execution units run in topological order
- lane-owned outputs are written forward
- visualization lanes are just more lanes in the graph
- `LanesVisualization` reads the latest published visualization state at its own
  cadence

The intended ownership split is:

- `Timeline` stores the lane graph
- `TimelineExecution` derives executable lane units and runtime memory from that
  graph
- `TaskRunner` schedules those lane units
- `LanesVisualization` consumes the latest published visualization-ready state

This keeps visualization aligned with the general "everything is a lane" model.

## Structural implications

This direction assumes:

- one main lane graph inside `Timeline`
- a separate execution app module derived from `Timeline`
- lane-owned output memory managed by the execution module
- execution separated from sampling
- lane graph edits remaining cheap and local

The lane graph should stay easy to edit.

By contrast, DSP graph execution may remain more execution-optimized and more
expensive to rebuild on source changes.

## Immediate follow-up

The next major task is still lane execution itself:

- keep `Timeline` focused on lane-graph structure
- introduce `TimelineExecution` as the derived executable view of that graph
- make lanes independently executable in the context of that execution module
- keep lane graph topology updates local
- separate execution from sampling
- then return to visualization on top of that substrate

This visualization direction note is meant to preserve the current decisions
until the execution refactor is ready.
