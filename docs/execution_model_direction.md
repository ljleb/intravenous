# Execution Model Direction

This note is partially superseded by
[task_runner_execution_direction.md](./task_runner_execution_direction.md) for
task ordering, task graph updates, and execution-plan compilation. This
document remains relevant for broader runtime ownership between `Timeline`,
`IvModuleInstances`, and devices.

This note captures the current intended direction for runtime execution while
the application is being split into isolated app modules.

It should be preferred over older execution wording in
[timeline_lanes_design.md](./timeline_lanes_design.md) where the two documents
disagree. The lanes document remains the primary place for lane kinds and lane
model semantics; this document is about execution ownership and runtime
behavior.

## Core ownership

- `Timeline` should be the long-lived runtime substrate.
- `IvModuleInstances` should own DSP graph execution.
- `Devices` should own device discovery and callbacks.

The important consequence is that DSP execution should not be owned by a global executor object. Each iv-module instance should own its executable graph state, and the timeline should coordinate demand, wiring, and playback state around those instances.

## Timeline responsibilities

`Timeline` should eventually own:

- the lane graph
- playing / paused state
- the current realtime request frontier
- current playback position / seek-resume position
- lane-to-lane dependency ordering
- dynamic wiring between producers and consumers

`Timeline` should not own DSP graph internals. It should coordinate execution, not become the owner of every DSP graph instance.

`Timeline` should also remain the authority for realtime transport state.
Device callbacks may request work, but they should not become the source of
truth for timeline position or playback advancement.

## DSP instance responsibilities

`IvModuleInstances` should eventually own:

- executable DSP graph instances
- per-instance execution state
- per-instance buffering
- push-ahead production logic

DSP instances should try to produce audio as fast as useful, then stop scheduling more work when they are sufficiently ahead of current demand. The goal is low latency without unnecessary work.

## Devices as lanes

Devices should stop belonging to executor-related classes.

Instead:

- one output device is selected as the master output device in application settings
- devices are exposed to the timeline as realtime lanes or endpoints through bridges
- the user wires devices dynamically through the timeline

Other output devices consume streams resampled to the master device rate.

## Global indexing direction

We do not want to globally index every produced sample block.

The preferred direction is to treat the global index as the current realtime
request index:

- realtime outputs advance this frontier
- DSP instances try to stay ahead of it
- compiled outputs may still support explicit `block(index)` style access

This keeps bookkeeping lighter while still giving compiled outputs a stable
addressing model.

This choice is intentional. The main alternative is to index every produced
block globally across all lanes. That would make random access and bookkeeping
uniform, but it pushes too much overhead into the runtime and creates a stronger
illusion that all lanes advance in lockstep. That is not a good fit for the
intended future where lanes may run at different rates and block sizes.

So the intended split is:

- realtime lanes are coordinated around the current request frontier
- compiled lanes may expose direct indexed access
- the runtime does not maintain a giant global ledger of every produced block

## Time and rate model

Not all lanes are expected to run at the same sample rate or block size in the future.

Assumptions:

- the rate ratio between any two lanes is always a power of 2
- some lanes may process faster or slower than others
- not all lanes need to advance at the same time
- graph inputs, graph outputs, and lane connections may eventually run at over-
  or under-sampled rates relative to one another

This means the runtime should be designed around a shared time protocol, not around lockstep advancement of every lane.
The API shape should leave room for timeline-owned scheduling, rate conversion,
and block-size adaptation rather than assuming one global realtime lane format.

## Paused / playing behavior

Desired behavior:

- when paused, realtime streams keep advancing
- when paused, compiled streams remain pinned at a fixed playback location
- when playback resumes, compiled playback jumps to the current realtime location, then continues normally

This keeps live inputs tied to the present while preserving a meaningful paused state for compiled playback.

## Realtime caution

The design should avoid blocking waits on realtime device callback threads.

If concurrency is introduced later, it should be done through bounded, realtime-safe scheduling and readiness tracking, not through arbitrary blocking in the audio callback path.

Realtime DSP graph instances should eventually execute as timeline-managed or
timeline-scheduled participants whose inputs and outputs are represented through
realtime lanes.

## Architectural implication

This direction supports the ongoing app-module split:

- `Timeline` remains a long-lived isolated app module
- `IvModuleInstances` becomes a long-lived isolated app module
- `Devices` becomes a long-lived isolated app module
- bridges connect them through events

This should replace the current executor-centered runtime shape over time.
