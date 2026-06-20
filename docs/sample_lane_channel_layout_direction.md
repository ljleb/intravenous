# Sample Lane Channel Layout Direction

## Scope

This document describes multichannel sample-lane design for lane nodes and
timeline lane execution only.

It does not cover DSP nodes or DSP graph ports. Those remain mono for now.

## Goals

- support automatic conversion of both channel type and sample memory layout
- keep the set of channel types closed and explicit
- precompute a per-edge conversion plan when lane connectivity changes
- execute a condensed conversion in one pass over one source region and one
  destination region at runtime
- preserve a typed lane-node API so nodes with static layout preferences can
  use specialized accessors without paying for dynamic abstraction in hot code

## Core Model

### Channel type

Channel type is a semantic property of a sample stream.

Initial closed set:

- `mono`
- `stereo`

The implementation should make it straightforward to add new channel types
later by extending the enum and conversion cases.

### Sample layout

Sample layout is a storage property of a sample stream.

Initial set:

- `planar`
- `interleaved`

### Channel layout

A sample stream is described by:

- a `ChannelTypeId`
- a `SampleStreamLayout`

Conceptually:

```cpp
enum class ChannelTypeId : unsigned int {
    mono,
    stereo,
    count,
};

enum class SampleStreamLayout : unsigned int {
    planar,
    interleaved,
};

struct ChannelLayout {
    ChannelTypeId channel_type = ChannelTypeId::mono;
    SampleStreamLayout sample_layout = SampleStreamLayout::planar;
};
```

## Conversion Model

### High-level pattern

Sample-lane conversion should mirror the existing event conversion model:

- closed type universe
- explicit direct conversions
- automatic path selection
- per-edge compiled conversion plan
- runtime executes only the selected condensed plan

Unlike event conversion, the runtime artifact is not a chain of event rewrites.
It is one block conversion procedure operating on one source block and one
destination block.

### Edge ownership

Conversion belongs to the consuming edge.

This allows:

- one source lane to feed multiple consumers
- different consumers to request different channel types
- different consumers to request different storage layouts

The producer stays in its own native representation.

### Runtime execution contract

The selected conversion procedure should use this shape:

```cpp
void convert(Sample const* src, Sample* dst, size_t frames);
```

Where:

- `src` points to one contiguous source memory region
- `dst` points to one contiguous destination memory region
- `frames` is the number of samples per channel

The selected plan already knows:

- source channel type
- source sample layout
- destination channel type
- destination sample layout

The conversion should happen once per prepared input block, not per sample
access during node execution.

## Initial Automatic Conversions

Both channel type and sample layout convert automatically.

Initial required cases:

- `mono -> mono`
- `mono -> stereo`
- `stereo -> mono`
- `stereo -> stereo`

combined with:

- `planar -> planar`
- `planar -> interleaved`
- `interleaved -> planar`
- `interleaved -> interleaved`

So the first runtime universe is the cross-product of:

- source type: `mono | stereo`
- source layout: `planar | interleaved`
- destination type: `mono | stereo`
- destination layout: `planar | interleaved`

The implementation should avoid hand-writing all cases independently. A single
templated conversion routine can be instantiated for every combination and
selected at runtime.

Conceptually:

```cpp
template<
    ChannelTypeId SrcType,
    SampleStreamLayout SrcLayout,
    ChannelTypeId DstType,
    SampleStreamLayout DstLayout
>
void convert_block(Sample const* src, Sample* dst, size_t frames);
```

This routine can use `if constexpr` to specialize:

- source reads
- channel-type adaptation
- destination writes

The runtime plan then stores a pointer to one of these precompiled
instantiations.

## Condensed Conversion

The selected conversion procedure should perform all required work in one pass.

Examples:

- `mono planar -> stereo interleaved`
  - read mono planar source
  - duplicate into left/right
  - write stereo interleaved destination

- `stereo interleaved -> mono planar`
  - read left/right from stereo interleaved source
  - downmix to mono
  - write mono planar destination

No temporary staged representation is desired in the common path.

## Lane-Node API Direction

### Static specialization

The typed lane-node context should keep exposing layout-specialized APIs when a
port config is statically known.

This is the reason for keeping the generated typed context model rather than
exposing only one fully dynamic view type.

If a port statically prefers planar layout, node code should be able to use a
planar-oriented API.

If a port statically prefers interleaved layout, node code should be able to
use an interleaved-oriented API.

### Generic fallback

A generic accessor should also always exist for dynamic cases, conceptually:

- `get(frame, channel)`

This allows ports whose config is not statically known to remain supported.

### Resulting API tiers

Sample input/output access should support three levels:

- planar-specialized access for statically planar ports
- interleaved-specialized access for statically interleaved ports
- generic `(frame, channel)` access always available

### Conversion timing

The conversion is not meant to happen inside every accessor call.

Instead:

- when preparing a node input for a block, the runtime selects the edge's
  precomputed conversion kernel
- that kernel fills the consumer-facing input region once
- the node then reads from its expected representation directly

From the node author's perspective the data simply appears in the requested
representation.

## Port Config Responsibility

Input and output sample configs should be able to express preferred sample
layout.

Default:

- `planar`

This keeps most lane-node definitions simple.

Nodes that want interleaved access can opt into it explicitly.

Channel type should not be forced into every lane-node type definition.
Channel type remains an authored graph property of the connected sample stream,
while the port config expresses storage preference.

## Lane-Level Semantics

Channel type is a property of a connected sample stream.

For this design pass, node processing code does not need to implement semantic
channel conversion internally. Semantic adaptation is handled on edges.

This means:

- a mono producer can feed a stereo consumer
- a stereo producer can feed a mono consumer
- the adaptation happens before the consumer executes

The node receives the representation it asked for.

## DSP Graph Boundary

This design does not yet extend DSP graph nodes or DSP graph ports to
multichannel sample streams.

Current boundary:

- lane nodes and timeline sample-lane execution gain channel-type and
  layout-conversion support
- DSP nodes and DSP graph ports remain mono

Any later DSP multichannel design can reuse the same closed-type plus
conversion-plan model if it still fits at that stage.
