# Sample Lane Channel Layout Repo Integration

## Scope

This document records the current concrete integration direction for multichannel
sample lanes in the repository.

It stays scoped to:

- lane nodes
- lane graph / timeline execution
- lane-facing JSON-RPC control

It does not yet extend DSP nodes or DSP graph ports. Those remain mono for now.

## New Shared Vocabulary

The channel/type conversion vocabulary should live in a dedicated sibling header:

- [channels.h](/home/abstrack/src/intravenous/src/intravenous/lane_node/channels.h)

This header should own:

- `ChannelTypeId`
- `SampleStreamLayout`
- `ChannelLayout`
- `channel_count(...)`
- `sample_storage_size(...)`
- the general sample channel/layout conversion plan machinery

The design is intentionally shared by both compiled and realtime sample ports.
Compiled sample configs must not be special-cased away from the same channel and
layout model.

## Initial Defaults

### Channel type

Initial closed set:

- `mono`
- `stereo`

Default channel type for newly created sample lanes:

- `stereo`

This default is only the current authoring/runtime default. A project-level
config default can be introduced later so the preferred default channel type is
user-configurable.

### Sample layout

Initial set:

- `planar`
- `interleaved`

Default sample layout preference for sample lane ports:

- `planar`

Nodes that want interleaved access can request it explicitly.

## Conversion Machinery

The channel/layout conversion machinery should use one runtime-selected plan per
sample edge.

Current intended model:

- source and target are described by `ChannelLayout`
- the runtime plan stores a precompiled conversion function pointer
- the conversion function takes:

```cpp
void convert(Sample const* src, Sample* dst, size_t frames);
```

- one contiguous source memory region
- one contiguous destination memory region
- one count of samples per channel

The implementation should use one generic templated conversion routine and
instantiate the finite cross-product of cases ahead of time.

For the initial universe:

- source channel type: `mono | stereo`
- source layout: `planar | interleaved`
- destination channel type: `mono | stereo`
- destination layout: `planar | interleaved`

The runtime only selects one of these precompiled kernels.

## Where Layout Preference Lives

Sample port configs should carry layout preference for both:

- realtime sample inputs/outputs
- compiled sample inputs/outputs

This is about memory layout only, not semantic channel type.

Channel type remains an authored property of the sample lane instance rather
than a property that every lane-node type definition must spell out.

## Lane Graph / Timeline Model

The lane graph currently stores lane records without explicit sample channel
type. The intended extension is:

- `LaneRecord` gains optional sample-lane channel metadata
- `TimelineLaneUpsert` exposes the same information
- modules creating sample lanes pick the initial channel type

For now, the default should be `stereo`.

`TimelineExecution` should then use that lane-level channel type to:

- size realtime sample storage
- size compiled sample cache chunks
- choose edge conversion plans

## Lane-Node Context Direction

The typed lane-node context should keep paying off for statically declared
layouts.

Expected direction:

- statically planar sample ports get planar-oriented accessors
- statically interleaved sample ports get interleaved-oriented accessors
- a generic `(frame, channel)` accessor remains available for dynamic cases

The conversion is not meant to happen on every accessor call.

Instead:

- edge conversion is prepared when connectivity changes
- block conversion happens once when preparing a consumer input block
- the lane node then reads the representation it requested

## JSON-RPC Follow-up

The UI will need a way to change the channel type of an existing sample lane
instance.

This implies a future JSON-RPC path that:

- targets a lane node instance
- changes its authored sample channel type
- causes the surrounding lane graph / timeline execution structures to
  synchronize to the new lane shape

This is separate from the current layout preference on node port configs.

### Important distinction

- channel type is lane-instance authored state
- sample layout preference is node-port configuration

## Non-scope Reminder

This design pass does not yet cover:

- DSP node multichannel ports
- DSP graph multichannel lowering
- project persistence of sample lane channel type

Those will need follow-up design once the lane-node and timeline side is
landed.
