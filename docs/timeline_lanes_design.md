# Timeline Lanes Design Notes

This document captures the current working design for Intravenous timeline
lanes. It is intentionally a design checkpoint, not a final specification.

For current runtime execution ownership and playback-direction decisions, also
see [execution_model_direction.md](./execution_model_direction.md).

Some older execution wording in this document is superseded by that newer note.
This document should remain the primary place for lane kinds, lane semantics,
and lane-model structure.

## Core Model

Everything time-varying or controllable is represented as a lane.

A lane is both the user-visible object and the node instance:

- A lane instance has one lane type.
- A lane instance has one output.
- A lane instance may have zero or more typed inputs.
- A lane instance may own data, such as sparse typed events or dense samples.
- A lane instance may generate its output from its inputs.
- A lane instance has stable in-memory identity, and later a persistent project
  identity.

There is no separate lane-node editor. Lane views are the editor.

## Lane Domains

Lane ports belong to one of two domains:

- Compiled lanes.
- Real-time lanes.

Compiled ports are deterministic, random-access, span-addressable, and usually
persistable. They are used for stored or generated timeline data.

Real-time ports are live streams or endpoints. They include panel knobs,
physical controllers, DSP graph inputs and outputs, and output devices.

Lane nodes can mix compiled and real-time ports. The output domain defines the
node's execution style:

- Real-time output lanes are push-oriented. They should produce data forward in
  time and be scheduled or throttled by runtime demand rather than being
  modeled as purely random-access timeline queries.
- Compiled output lanes are push/invalidation-based. Edits, recording, imports,
  or dependency changes enqueue invalidated spans for regeneration.

The exact execution ownership and scheduling model is being revised. In
particular, realtime DSP execution is moving toward instance-owned execution
coordinated by the timeline, rather than a purely pull-shaped model. Lane-domain
semantics should stay stable even as that runtime execution model changes.

## DSP Nodes And Compiled Data

DSP nodes and lane nodes are different kinds of node:

- DSP nodes are real-time processors with compile-time graph structure.
- Lane nodes are compiled-data generators with runtime graph structure.

DSP nodes still need to be able to consume compiled random-access data when the
algorithm requires it. For example, a convolution node needs access to an IR
buffer by pointer plus size, and may need to random-access or sweep that buffer
on demand.

This means the DSP node traits should eventually grow a way to declare compiled
resource inputs in addition to sample and event inputs. These inputs are not
ordinary real-time streams. They are prepared timeline-owned resources made
available to the DSP graph.

The important boundary is:

- The timeline owns and prepares compiled data.
- DSP nodes may consume prepared compiled data through explicit resource inputs.
- DSP nodes should not generate compiled timeline data directly from the audio
  callback.

Recording from DSP output into compiled timeline data is a system operation, not
a DSP node producing compiled lane output.

## Crossing Domains

Crossing between real-time and compiled domains is modeled by lane nodes whose
input and output port domains differ.

- Real-time to compiled covers recording.
- Compiled to real-time covers playback feeds and prepared resources.

Examples:

- A panel knob lane can be recorded into a compiled automation lane.
- A physical MIDI keyboard lane can be recorded into a compiled MIDI event lane.
- A compiled sample lane can feed a real-time parameter input through prepared
  playback data.
- A compiled IR buffer can feed a convolution DSP node through a compiled
  resource input.

## Lane Types

A lane type declares:

- Compiled sample inputs.
- Compiled event inputs.
- Real-time sample inputs.
- Real-time event inputs.
- One output.
- Optional owned data.
- Optional custom UI.
- A generation callback.

Sparse buffers always reuse the existing event typing system. There is no
separate vertex type fallback. A sparse sample curve is a sparse event buffer
whose event type contains sample values.

Interpolation is not metadata on a sparse buffer. Interpolation is performed by
a lane type that consumes sparse typed events and generates dense samples or a
sample-addressable representation.

A possible shape:

```cpp
struct SmoothSampleLane {
    auto compiled_sample_inputs() const;
    auto compiled_event_inputs() const;
    auto realtime_sample_inputs() const;
    auto realtime_event_inputs() const;
    static auto output();

    void generate(TimelineGenerateContext<SmoothSampleLane>& ctx) const;
};
```

`TimelineGenerateContext<T>` should be typed by lane type, like
`TickBlockContext<T>`. Internally, it may be a typed view over an untyped context.
The context owns the requested output span information; a separate request set
argument should not be necessary. The context exposes:

- `start_index()`.
- `count()`.
- Four input view spans matching the four declared input buckets.
- `out()`, whose type follows the node's output declaration when statically
  known, or is a variant when the node declares dynamic output.

Any block view returned by a context accessor is call-scoped. Lane nodes must not
store these views across `generate()` calls. Persisting generated data,
intermediate storage, scratch buffers, and published spans is the job of the lane
processor/executor wrapper.

## Inputs And Contributors

A lane input may have multiple contributors.

For sample inputs, contributors are summed pointwise deterministically.

For event inputs, contributors are concatenated and sorted or merged using the
same style of k-index merge currently used by real-time DSP event merging.

Therefore the connection model is:

```text
many source lane outputs -> one lane input
```

Fanout is also allowed:

```text
one source lane output -> many target lane inputs
```

The compiled dependency graph should be a DAG. Real-time edges can reach DSP
graphs, devices, and other runtime endpoints through explicit lane/DSP bridge
nodes installed during graph build and cleaned up through graph lifecycle hooks.

## Lane Change Events And Views

The timeline should be the owner of lane change events.

Lane views should react to timeline-owned lane-change notifications through
isolating bridges, rather than being coupled directly to DSP-specific lane
management modules. This keeps lane ownership, lane mutation, and lane-view
projection separate.

## Outputs

Each lane has exactly one output.

Multiple outputs are not part of the core semantic model. If an operation
eventually needs multiple outputs for performance or memory reasons, prefer
multiple sibling lanes with shared inputs and introduce shared cached
intermediates later.

## Parent/Child Metadata

Parent/child relationships are organizational metadata, not execution semantics.

They can be useful for:

- Grouping related lanes.
- Search context.
- Hiding internal helper lanes.
- Presenting composite lanes.
- Batch operations.

Execution is determined by lane input connections, not by parent/child
relationships.

## Custom UI And Composite Lanes

Lane types may define custom UI.

A lane type may also forward the UI of one of its child/internal lanes. This
allows composite lanes whose top-level output is processed data while the
surfaced editor is an internal editable sparse buffer or another child lane.

Example:

```text
Editable sparse sample events -> smoother -> scaler -> composite output
```

The composite lane can expose the sparse event editor at the top level while
still outputting the processed result.

## Logical And Concrete Controls

Graph input controls are represented with knob lanes and graph input lanes.
There is no separate logical input lane for sample controls.

A logical panel control is a knob lane. Each concrete graph input has its own
concrete knob lane, and the concrete graph input lane is an organizational child
of that concrete knob lane. Parent/child relationships do not imply execution;
they only help views avoid clutter and surface the user-facing control by
default.

Default logical-to-concrete behavior is ordinary lane wiring:

```text
LogicalKnobLane -> ConcreteKnobLane[0] -> GraphInputLane[0]
LogicalKnobLane -> ConcreteKnobLane[1] -> GraphInputLane[1]
LogicalKnobLane -> ConcreteKnobLane[2] -> GraphInputLane[2]
```

Concrete knob lanes have an override/control input. If that input is connected,
the concrete knob follows the upstream lane. If it is disconnected, the concrete
knob owns and emits its local value while its output remains connected to the
graph input lane.

A concrete self-control operation is therefore just a lane graph edit:

```text
disconnect LogicalKnobLane -> ConcreteKnobLane[1]
```

Clearing the divergence restores:

```text
connect LogicalKnobLane -> ConcreteKnobLane[1]
```

A logical knob can display as inactive if it has no outbound connections and no
longer effectively drives anything.

The VS Code panel is a compact specialized lane view over these real-time lanes.

## Knobs

Knobs are real-time lanes with a compact knob UI. They are not architecturally
special; the panel simply chooses to render some knob lanes inline with the DSP
graph interface instead of in the main lane view.

Knob lanes and graph input lanes are ordinary lane nodes with stable semantic
identity strings. Tags are general lane metadata exposed through the lane-node
trait/type-erased surface so views can filter/group lanes without hard-coded
node categories or enum-based behavior specialization.

They can connect to real-time or compiled lanes according to normal lane domain
rules. To make a knob gesture persistent, route it through a real-time-to-
compiled recording lane:

```text
PanelKnobLane -> recording operation -> compiled automation lane
```

If compiled automation drives a parameter, the knob should dynamically follow
that parameter's current value. Grabbing the knob is a temporary real-time
override; releasing it restores ownership to the existing automation/connection.

## Event And Sample Automation

Both DSP sample inputs and DSP event inputs can receive automation.

There is no conceptual distinction between parameter inputs and sample inputs.
They differ by processing rate and resampling behavior, not by identity.

DSP sample inputs consume sample streams or prepared sample data. DSP event
inputs consume typed event streams or buffers.

## Devices And Graph Instances

Output devices should eventually become real-time output device lanes.

The current output device mixer already resembles this model:

- It owns a logical audio device.
- It registers channel sinks.
- It mixes connected sample buffers.
- It submits blocks to the device.

Future shape:

```text
Realtime graph output lanes -> OutputDeviceLane inputs
```

Changing output device lane connections should dynamically update mixer routing.

Recording DSP output becomes:

```text
DSP output realtime lane -> recording operation -> compiled sample lane
```

Multiple DSP graph instances may eventually expose real-time input and output
lanes, allowing graph instances to be connected or recorded through the timeline.

Before multiple DSP executors are viable, output device ownership and mixer
routing need to move out of individual node executors and into a shared
timeline/lane/device layer. Output devices, microphones, and other system devices
cannot be duplicated per executor.

## Lane Views

VS Code lane views are filtered projections over lanes and connections.
They are long-lived server-side objects, not just polling queries. The client
opens a view with a filter/order and then reports viewport facts such as start
index and visible lane count. The server owns the ordered projection for that
view and sends notifications when lane graph or lane metadata changes affect
the displayed lanes.

The transport layer should only deliver view snapshots/updates. It should not
own filter semantics or lane graph inspection. The lane view service sits
between the lane graph/processor and the socket server:

- `LaneGraph` owns lanes and connections.
- `LaneProcessor` owns execution, caches, and regeneration.
- `LaneViewService` owns active view projections and dirty view tracking.
- `SocketRpcServer` sends view updates as JSON-RPC notifications.
- The VS Code client owns physical presentation and reports viewport state.

Polling remains useful for opening, restoring, scrolling, and resizing a view.
Regular lane changes should dirty affected views and be surfaced through
notifications instead of requiring the client to repeatedly poll.

Multiple views can be open at once with independent filters:

- Latest touched lanes.
- Current source selection.
- Stale or disconnected lanes.
- Concrete overrides.
- MIDI-targetable instruments.
- Output devices.
- Pinned performance controls.

Views should show:

- Lanes.
- Lane connections.
- Logical-to-concrete default links.
- Overrides.
- Connections to lanes outside the current filter.
- Inbound/outbound connection summaries.
- Follow-connection actions.

Derived views are postponed, but view result sets can later feed batch
operations such as reconnecting stale lanes, retargeting connections, moving
overrides, or connecting selected controllers to selected targets.

## Search Metadata

Every lane should track searchable metadata:

- Lane id.
- Domain.
- Lane type.
- Output type.
- Target descriptors, if it maps to DSP graph I/O.
- Source spans, if relevant.
- Last touched timestamp.
- Last edited timestamp.
- Connected, disconnected, or stale status.
- Parent/child grouping metadata.
- Tags and pins.
- Upstream/downstream connection info.

The latest-touched view is an ordered filter over this metadata.

## Identity

Lane identity is separate from DSP logical node identity.

Runtime lane IDs can be generated cheaply with an atomic or generational counter.
Later project persistence can assign stable saved IDs.

`Timeline` owns the persistent lane graph/lane executor. Reconciliation after a
DSP graph reload must reuse existing lane nodes by semantic identity instead of
rebuilding the lane graph from scratch.

Connections to DSP graph inputs store rich target descriptors so they can
reconnect after reload:

- Logical identity.
- Optional concrete member ordinal.
- Port kind.
- Port ordinal/name/type.
- Module/source hints.

## Port Kind

The repo already has port kind concepts in more than one place. These should be
centralized before adding more lane APIs.

The shared port kind should be used for:

- Graph introspection.
- Control targets.
- Lane ports.
- Server APIs.
- VS Code display.

## Initial Implementation Direction

The safest first slice is the structural substrate:

1. Centralize the shared port kind.
2. Introduce `LaneId`, `LaneTypeId`, lane endpoint, and lane connection types.
3. Represent panel controls with knob lanes and graph input lanes.
4. Represent inheritance and concrete self-control as lane connections.
5. Add a lane item query API for VS Code filtered views.
6. Expose latest-touched metadata.
7. Add the first compiled lane storage: a sparse event buffer using existing
   event typing.
8. Add recording and compiled-to-real-time feed/resource operations.

This gets today's controls onto the future lane model before building the full
compiled timeline generation engine.

The immediate lane processor slice should:

1. Store lane nodes with stable `LaneId` values.
2. Store connections in both input-to-contributors and output-to-dependents maps.
3. Use domain-aware input port ids, not arbitrary DSP graph metadata.
4. Mark edited lanes dirty and cascade invalidation through dependents.
5. Queue affected compiled-output lanes for regeneration work.
6. Leave actual DSP graph ownership outside the lane graph.
