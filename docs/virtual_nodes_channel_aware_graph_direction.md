# Virtual Nodes and Channel-Aware DSP Graph Direction

## Status and scope

This document records the target direction for replacing the current
`GraphBuilder::multi_channel(...)` mechanism with channel-aware DSP graph
ports and virtual nodes.

It extends the channel type/layout and conversion vocabulary established for
timeline lanes. It intentionally changes the DSP graph itself: DSP node ports
are no longer implicitly mono.

This is a design direction, not a compatibility plan. `multi_channel` and its
generated-port-name protocol are to be removed after callers and tests have
migrated.

## Terminology

The former term **logical node** is replaced by **virtual node** in this
model. This is more specific: a virtual node is the source-authored graph
object whose implementation may contain one or more executable nodes.

| Term | Meaning |
| --- | --- |
| **Concrete node** | One executable node instance run by the DSP graph. |
| **Concrete port** | An input or output on a concrete node, with a declared channel type/layout. |
| **Virtual node** | The graph-facing object associated with one source-authored named lvalue that receives a node-reference implementation. It owns the mapping from its virtual ports to concrete nodes and ports. |
| **Virtual port** | One graph-facing and UI-facing port on a virtual node. It may compose several concrete ports created by tiling. |
| **Tiled node** | A virtual node whose fully-mono concrete node type is instantiated once for each member of a requested channel type. A tiled node is not a concrete node and is not a separate module-author-facing value. |
| **Tile** | One concrete-node member of a tiled node. For a mono node tiled to stereo, the left and right concrete instances are the two tiles. |
| **Tiling** | Creating the tiles that implement a tiled node. |
| **Virtual-port mapping** | The builder-owned relation from one virtual port to its concrete port(s), including the channel member handled by each tile. |
| **Lane binding** | A `GraphInputLanes` association between one managed timeline lane and one virtual input or output port. It is never an association with an individual tile. |
| **Channel conversion** | A planned edge operation that converts between compatible channel types/layouts while preserving each endpoint's declared representation. |

Source spans, source identities, lane state, sidepanel controls, and
introspection attach to virtual nodes and virtual ports. Concrete members are
implementation detail unless a diagnostic explicitly asks to expose them.

## Authored identity, references, and membership

A bare `g.node<T>()` expression creates concrete-node implementation data and
returns a node reference. It does not, by itself, create an authored virtual
node. The source rewriter establishes virtual-node identity when a named
lvalue receives that reference. It gives an uninitialized node-reference
lvalue its stable declaration identity and wraps its initializer or assignment
right-hand side with the source-annotation operation for that identity.

Conceptually:

```text
auto filter = g.node<Filter>();

source-authored lvalue `filter`
  -> stable virtual-node identity
  -> explicit membership/mapping to the concrete Filter implementation
```

This source annotation is an explicit declaration of membership; it is not a
later grouping heuristic. Source spans and type identities may be stored as
metadata on the virtual node, but they must never be used to discover, split,
or merge virtual nodes. Internal builder-generated nodes, unannotated
temporaries, constants, sums, packs, and unpacks have no authored virtual-node
identity unless an explicit source annotation associates them with one.

Node-reference values remain move-only. That is a runtime-reference rule which
keeps authored C++ assignment and aliasing behavior tractable; it is not an
exclusive-ownership rule for graph metadata. Moving a reference clears the
moved-from runtime handle, but does not remove virtual/concrete memberships
already recorded by source annotation.

Virtual/concrete membership is explicitly many-to-many. One virtual node can
have several concrete members, as a tiled node does, and one concrete node may
be a member of more than one virtual node when separately annotated authored
lvalues intentionally project it. The builder stores both directions:

```text
Virtual node A ---\
                    concrete node X
Virtual node B ---/
```

Repeated annotation of the same virtual-node/concrete-member relation
deduplicates it. It must not erase membership belonging to another virtual
node. The forward virtual-node record and its virtual-port mappings are
authoritative for sidepanel, lane, persistence, and introspection projection;
the inverse concrete-to-virtual relation supports build metadata and
diagnostics.

## Channel declarations

`InputConfig` and `OutputConfig` gain optional channel type and sample-layout
information. An absent declaration means mono, using the normal default sample
layout.

The declaration is a representation request, not a statement about the
semantic role of a port. There is no port taxonomy such as "parameter" versus
"audio", and no corresponding connection prohibition.

Every node's `inputs()` and `outputs()` must be evaluatable in a constexpr
context. The config objects remain ordinary values; the node trait layer can
evaluate the returned config arrays to derive channel type, layout, width, and
other compile-time facts. Node traits should reject definitions that cannot
satisfy this requirement.

The native width of a concrete node is the maximum channel count of its sample
input and output declarations. Channel type—not just width—is retained so the
builder can plan the appropriate conversion.

## Node creation and tiling

`g.node<Node>()` retains `Node`'s native concrete-port interface.
`g.node<Node, RequestedChannelType>()` requests a channel-aware authored-node
interface backed by one or more concrete instances. The channel type is
explicit in the initial model; it is not an implicit graph-wide default.

A virtual node is builder and graph metadata, not a distinct value returned to
module authors. The expression still returns the familiar node-reference
interface: it is called to connect named inputs and is used to obtain named
output references. A source-authored binding such as
`auto const filter = g.node<OnePoleFilter, stereo>();` has one corresponding
virtual node, even when its implementation has several concrete members.

The returned reference does **not** expose or require indexing the concrete
nodes. It presents the promoted channel type at every sample port. For a node
whose native declarations are all mono:

```text
native OnePoleFilter             g.node<OnePoleFilter, stereo>()

input  "source": mono            input  "source": stereo
input  "cutoff": mono            input  "cutoff": stereo
output "main":   mono            output "main":   stereo
```

The normal node-call and output-reference DSL remains the interface:

```cpp
auto const filter = g.node<OnePoleFilter, stereo>();
filter("source"_P = stereo_source, "cutoff"_P = stereo_cutoff);
auto filtered = filter["main"_P];
```

`filtered` is a stereo channel-aware port value. Selecting
`filtered[stereo::left]` selects a channel of that port value (equivalently,
the `"main"_P[stereo::left]` form); it does not select a concrete filter
instance. Normal node-to-node wiring therefore does not expose tiling.

The initial implementation permits `g.node<Node, RequestedChannelType>()`
only when every sample input and output declared by `Node` is mono. It creates
one independent concrete `Node` instance for every member of the requested
channel type. For each member `c`, every mono concrete input/output port maps
to channel `c` of the corresponding promoted virtual port. A mono value may
broadcast into such a promoted port, and compatible channel representations
convert at the consuming edge.

This restriction deliberately rejects nodes with mixed mono and native
multi-channel sample ports. For example,
`g.node<MixedMonoStereoNode, surround_5_1>()` is invalid initially. Channel
count divisibility does not say whether the mono port should broadcast, become
5.1, or be shared, nor how a native stereo port partitions into 5.1. Those are
future explicit per-port mapping rules, not defaults inferred from width.

Event-port policy must likewise be explicit before promoted eventful nodes are
enabled: event inputs need a broadcast/partition rule and event outputs need
a merge rule. The first promoted-node implementation may reject event ports.

Later extensions may allow native multi-channel concrete nodes. The builder
will then compare the requested channel count with the concrete node's native
width:

- equal widths: materialize one concrete node;
- requested width is a multiple of native width: tile concrete node instances;
- otherwise: reject construction with a clear diagnostic.

For the initial fully-mono case, native width is one and the mapping described
above is direct. Wider native-node tiling requires an explicit channel
partition/mapping contract; in particular, a divisible width alone must not
authorize a stereo algorithm to process arbitrary pairs of a 5.1 signal.

Tiling is a graph-builder implementation operation. It must be recorded as
explicit virtual-to-concrete membership and virtual-port-to-concrete-port
mappings, not reconstructed from generated port names, source spans, scopes,
or adjacency.

## Connections and conversion

Virtual-port connections lower into concrete connections, broadcasts, and
channel-conversion operations.

- Matching type/layout forwards directly.
- Compatible types with different representations use the channel conversion
  graph at the consuming edge. For example, a left/right stereo source can
  connect to a mid/side stereo port without either endpoint performing manual
  conversion.
- Mono values can broadcast into the members of a tiled virtual port.
- A virtual port connected to a timeline lane remains one channel-aware lane,
  rather than one lane per concrete member.
- Incompatible channel counts or conversion paths are errors; the builder must
  not silently flatten, select an arbitrary channel, or invent a reduction.

For tiled-node ports, the builder first attempts a direct tile-to-tile port
mapping. Two tiled nodes with the same channel type can therefore connect
without an intermediate multi-channel buffer: each matching channel member is
an ordinary mono concrete edge. A direct mapping is not valid merely because
two channel types have the same count; left/right stereo and mid/side stereo,
for example, still require a representation conversion.

When a tiled virtual port must connect to a native multi-channel concrete port,
or vice versa, lowering inserts internal channel adapter nodes rather than
adding tiling-specific behavior to ordinary graph nodes:

```text
ChannelPack<C>:    C::channel_count mono inputs -> one C-typed planar output
ChannelUnpack<C>:  one C-typed input -> C::channel_count mono outputs
```

`ChannelPack<C>` gathers independently allocated mono concrete streams into
the channel-aware stream required by a native consumer. `ChannelUnpack<C>`
projects a native channel-aware producer into the independent mono streams
required by tiled consumers. The compiler may use aliasing/views where a
layout permits it and may later fuse packing, unpacking, channel conversion,
and planar/interleaved layout conversion, but those optimizations preserve the
same adapter semantics.

These are builder-generated internal nodes. They participate in ordinary root
graph compilation, scheduling, buffer lifetime, latency, and SCC handling,
but do not appear as authored nodes, sidepanel entries, or lane endpoints.

The producer retains its declared representation. A consumer receives the
representation declared by its concrete port. This follows the existing lane
conversion principle that conversion belongs to the consuming edge.

## Concrete graph execution

The concrete DSP graph must carry channel-aware sample streams. It cannot
remain a scalar-only graph once a concrete node can declare a stereo, mid/side,
or other channeled port.

`InputPort`, `OutputPort`, and the tick/tick-block contexts should preserve the
existing convenient mono API for mono configurations. They also need generic,
template-based channel traversal suitable for both:

- channel-first processing: each channel, then its samples;
- frame-first processing: each sample frame, then its channels.

The public API should not grow a separate member-function family for every
channel type or storage layout. For constexpr node port configurations, the
context derives the required compile-time channel/layout information from the
node's config array and port index/name. A change from mono to stereo then
causes mono-only node code to fail locally at compile time, while genuinely
generic traversal code remains valid.

Every concrete DSP node inserted through `g.node<T>()` must satisfy this
constexpr sample-port contract. Runtime-shaped sample-port declarations are
not a second concrete-node path. `TypeErasedNode` remains runtime storage and
dispatch machinery for an already-built graph, but is not itself an insertable
DSP node type.

### Discovered VST nodes

VST probing is not an exception to the constexpr node-config contract. The
runtime-shaped VST wrapper is removed; a future VST discovery utility emits a
concrete C++ node type for each discovered plugin schema.

The generated type owns only compile-time schema data:

- constexpr input, output, and MIDI port declarations;
- stable generated type/name identity;
- plugin descriptor data needed to create the runtime instance.

It delegates initialization and block processing to shared VST runtime
utilities. Those utilities own live plugin state, parameter binding, audio/MIDI
marshalling, and the common `tick_block` implementation. Generated wrappers
must not duplicate that processing logic.

The DSL creates a discovered wrapper by type, for example
`juce::vst<GeneratedPluginNode>(g)`, rather than by probing a plugin during
`g.node(...)`. This moves schema discovery into a generation/build step and
makes the generated node satisfy the same constexpr-port contract as every
other DSP node.

## Sidepanel and lane semantics

The sidepanel displays virtual nodes and virtual ports, not concrete nodes or
tiles. An authored `g.node<MonoFilter, stereo>()` binding therefore contributes
one sidepanel node and one sidepanel entry for each of its authored ports,
even though its implementation contains two mono filter tiles. Tile count,
tile order, and concrete node IDs are implementation detail and must not
appear in UI labels, persisted UI state, or lane identities.

For a mono concrete node tiled into a stereo virtual node:

- each virtual input/output port is displayed as stereo;
- lane-connected virtual inputs or outputs create and use one stereo lane,
  not one mono lane per tile;
- source navigation selects the virtual node/port source span and remains
  independent of the number of tiles;
- replacing or rebuilding concrete tiles updates the virtual-port mapping while
  preserving the sidepanel control and lane binding when virtual identities
  remain the same.

`GraphInputLanes` manages lane bindings at the virtual-port boundary. Its
authoritative endpoint key must be a virtual-port address, conceptually:

```text
{ module instance, virtual node identity, port kind, virtual-port ordinal }
```

It must not use a concrete member/tile ordinal as part of authored lane
identity. The current logical/concrete key split is transitional and is
replaced when graph-input state moves to virtual nodes and ports.

At graph rebuild time, the builder exposes a virtual-port mapping to
`GraphInputLanes`: the virtual port's channel layout, default/control data,
and the concrete input or output endpoints for each channel member. A lane
source connected to a stereo virtual input therefore lowers through the
mapping to the left and right mono concrete inputs. Conversely, a tiled
virtual output lowers to one channel-aware lane source before it reaches the
timeline. `GraphInputLanes` requests and retains one lane; the builder owns
all tile routing and keeps it invisible to the lane manager.

The same rule applies to all virtual-port state—default values, overrides,
connections, source spans, persistence, and sidepanel controls. That state is
addressed by the virtual port and has its declared channel layout. It is not
duplicated per tile. A per-channel control policy, if needed beyond the
ordinary channel-aware lane value, must be introduced explicitly at the
virtual-port level rather than by exposing tiles.

Later native-channel support follows the same boundary rule: a native
multi-channel concrete port is represented by one virtual port whose displayed
channel type/layout is its effective virtual-boundary representation.

## Replacement of current mechanisms

The following current mechanisms are transitional and must be removed rather
than preserved as a second channel model:

- `GraphBuilder::multi_channel(...)`;
- the current `ChannelRefs` role as an array manufactured by repeated lambda
  execution (a channel-aware port value may remain, but must derive from a
  virtual-port mapping);
- generated `__mono_center_*`, `__stereo_left_*`, and `__stereo_right_*` port
  names;
- channel-family reconstruction by parsing those names;
- grouping repeated concrete nodes through source annotation or scope
  heuristics.

`ChannelRefs` may remain as a user-facing channel-stream/reference value, but
its meaning must be derived from virtual/concrete port mappings rather than
from `multi_channel`.

## Implementation order

The work is organized as atomic changes, listed with prerequisites before their
dependents. Independent items may be completed in either order. No
compatibility layer for `multi_channel` or generated channel names is added
during this work.

### Atomic work items

- **Concrete channel execution.** Complete the shared descriptor/runtime-ID
  bridge, constexpr concrete port declarations, channel-aware buffers and tick
  contexts, and explicit channel-conversion semantics. This is the concrete
  execution contract on which all other channel work relies.
- **Channel boundary adapters.** Add generic internal `ChannelPack<C>` and
  `ChannelUnpack<C>` concrete nodes, including their ordinary root-graph
  execution and tests. They are independent of virtual nodes and the authored
  node-reference DSL.
- **Native channel-aware node interface.** Make the existing node-reference,
  node-call, output-access, arithmetic, and public-port DSL carry a reference
  to one native C-typed concrete port. Prove direct native multi-channel node
  wiring end-to-end.
- **Virtual graph metadata.** Replace inferred logical grouping with explicit
  virtual-node identities, virtual ports, and initial virtual-to-execution-
  node/port mappings. Source annotation on an authored lvalue records an
  identity and explicit membership; insertion alone does not. Membership may
  be many-to-many, but it is never reconstructed from names, source spans,
  scopes, adjacency, or type matching.
- **Tiled node insertion, values, lowering, and old-model removal.** Add
  `g.node<T, C>()` through `TiledNode<T, C>` for constexpr fully-mono sample
  nodes, with one execution tile per member of `C` and a channel-aware form of
  the existing node-reference interface. Its channel-aware port values map
  members of `C` to separate mono execution ports; matching tiled endpoints
  lower to direct mono edges, and native/tiled boundaries lower through
  pack/unpack and conversion operations. These behaviors are one atomic item:
  none is meaningful without the others. In the same migration, delete
  `GraphBuilder::multi_channel`, callback-manufactured `ChannelRefs`, generated
  `__mono_*`/`__stereo_*` names, their parser, and all fallback tests; migrate
  every caller directly.
- **Virtual-node projection.** Make source introspection and the sidepanel
  expose virtual nodes and virtual ports, hiding execution nodes and tiles.
- **Virtual lane-state ownership.** Re-key `GraphInputLanes`, project
  persistence, and RPC state at the virtual-node/implementation-port boundary.
  Preserve override, follow, lane, and disconnected behavior, but a tiled
  implementation port has one C-typed state/binding rather than one per tile.
  The builder resolves it to execution ports during rebuild.
- **Fixture and rebuild migration.** Convert modules and integration fixtures,
  then prove source navigation, lane bindings, overrides, and channel mappings
  survive rebuilding a tiled node.

Native multi-channel tiling, mixed-width node types, eventful promoted nodes,
shared/per-channel control rules, richer channel types, and fused
pack/unpack/conversion operations are later independent design items. They are
not inferred from channel counts and do not block the initial fully-mono tiled
node model.

## Contract tests

The test suite should define this model before broad migration:

- node traits reject non-constexpr `inputs()` or `outputs()`;
- native channel type/layout derives from declared configs;
- a fully mono concrete node tiles into a promoted wider node-reference
  interface;
- every promoted sample port maps one requested channel to one concrete mono
  member, while mono values broadcast deliberately;
- matching tiled ports lower to direct mono concrete edges, while native/tiled
  boundaries lower through `ChannelPack<C>` and `ChannelUnpack<C>`;
- equal channel counts with different representations require conversion and
  do not qualify for direct tile-to-tile mapping;
- the sidepanel and `GraphInputLanes` expose one virtual-port control and one
  channel-aware lane for a tiled port, never one control/lane per tile;
- rebuilding a tiled node preserves a virtual-port lane binding while replacing
  its concrete-port mapping;
- mixed native mono/multi-channel sample ports are rejected by the initial
  promoted-node model;
- later native-channel extensions materialize once at matching requested width
  and reject missing explicit partition mappings;
- matching streams forward and compatible layouts convert at the edge;
- virtual mono ports tiled into stereo present one stereo sidepanel entry and
  one stereo lane;
- source spans, lane identities, and lane connections survive rebuilds without
  depending on concrete-member ordering.
