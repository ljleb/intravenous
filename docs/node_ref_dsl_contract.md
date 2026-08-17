# Node-Reference DSL Contract

## Contexts and identities

The DSL has two different node contexts.

- A **virtual node** is static/authored graph metadata. It is created when a
  source-authored declaration is annotated. It owns source identity, source
  spans, virtual ports, and explicit membership of node bundles. It is not an
  executable node and it is not a value returned by `GraphBuilder::node`.
- A **node reference** is a move-only builder-time value. It refers to exactly
  one node bundle in one `GraphBuilder` and creates or inspects graph wiring.

A node bundle is exactly one concrete node, one tiled set of concrete nodes,
or one subgraph node. Concrete nodes and individual tiles are execution
implementation details. Virtual-node-to-bundle membership is explicit and
many-to-many; it must never be inferred from names, source spans, types, tile
order, or adjacency.

## Public node-reference types

```cpp
NodeRef
TypedNodeRef<Node>
TiledNodeRef<Node, ChannelType>
```

`NodeRef` is the move-only common base and contains an erased bundle handle.
`TypedNodeRef<Node>` adds `Node`'s constexpr port shape for a concrete bundle.
`TiledNodeRef<Node, C>` adds `Node`'s constexpr port shape, the promoted
channel type `C`, and typed access to its concrete member bundles.

The CRTP implementation helper is internal. It is not a DSL-facing type.
Typed refs derive from `NodeRef`, but an erased handle for an lvalue is made by
`node_ref()`, not by copying or slicing a node ref.

## Common bundle API

Every `NodeRef` operation is defined through `NodeBundle` operations. Callers
must not inspect a bundle kind or call `single_concrete_node()`. The erased
bundle API supplies counts, name lookup, layouts, lowering to concrete ports,
topology/concrete-member traversal, import/remap, and annotation storage.

All node refs support ordinal and named sample/event input wiring, sample/event
connection state, ordinal and named output lookup, event-output access, TTL,
source annotation, and diagnostics. A subgraph bundle supports the same common
operations.

## Typed additions

`TypedNodeRef<Node>` provides static named output types, `static_output<I>()`,
tuple `get<I>()`, and compile-time validation of calls.

`TiledNodeRef<Node, C>` provides promoted typed outputs and
`tiled[channel] -> TypedNodeRef<Node>` for an exactly matching member of `C`.
That selects a concrete node bundle. In contrast,
`tiled["out"_P][channel]` selects a channel of a promoted sample-port value.
Normal graph wiring uses promoted ports and does not require tile selection.

## Port values and lowering

`SamplePortRef` is the common untyped reference to one sample output. It may
refer to a concrete output, a bundle output, or a graph/subgraph boundary.
Typed sample-port refs extend it. A promoted tiled port is one logical bundle
output and additionally exposes channel selection.

`EventPortRef` is one event stream. Tiled event inputs broadcast one stream to
all tiles; tiled event outputs merge tile streams before producing one event
ref.

GraphBuilder lowers bundle ports to concrete ports only when creating concrete
edges, detach nodes, public outputs, or execution metadata. Native/tiled
boundaries use pack/unpack adapters. Matching tiled endpoints may lower to
matching mono edges only when their channel types, not merely their widths,
match.

## Expression rules

- `g.node<N>()` returns a concrete typed ref when `N` has preserved static
  shape, otherwise `NodeRef`.
- `g.node<N, C>()` returns `TiledNodeRef<N, C>` and is valid only for supported
  tiling mappings.
- `g.subgraph(...)`, embedding, and module loading return `NodeRef` to one
  subgraph bundle.
- `ref(...)`, `connect_input`, and `connect_event_input` connect positional or
  named inputs. Named resolution is bundle-level for erased refs and static
  where typed information is available.
- `ref[i]` and `ref["name"_P]` select outputs. Typed refs preserve output
  channel/layout information; erased refs return an erased bundle port.
- A node ref may implicitly convert to `SamplePortRef` **only when it has
  exactly one sample output**. The output name is irrelevant. It does not need
  to be named `"main"`.
- A node ref may supply `event_port()` **only when it has exactly one event
  output**. The event output name is irrelevant.
- `std::get<I>(ref)` is available only for typed concrete/tiled refs with a
  constexpr output tuple; it is not available on erased `NodeRef`.
- `tiled[channel]` selects a concrete typed node; `tiled[port][channel]`
  selects a promoted-port channel.
- Sample/event chaining operators require exactly one matching target input and
  produce the documented one-output/source/node chaining result.
- Arithmetic creates ordinary builder nodes. Typed ports preserve channel type
  and layout; incompatible channel representations fail rather than silently
  flattening or selecting channels.
- `~port` detaches a sample stream. Bundle outputs are lowered first.
- Public outputs accept ports or nodes under the same exactly-one-output rule;
  their virtual/bundle identity never becomes a tile identity.

## Non-negotiable constraints

- Node refs are move-only.
- A dynamic node ref always denotes one bundle, never a virtual node or an
  arbitrary collection of bundles.
- A tiled bundle is one bundle, not a collection of unrelated refs.
- Common DSL code has no concrete-node assumption.
- Event ports are not channelized.
- Internal conversion, pack, and unpack nodes are not authored virtual nodes.
- Runtime execution consumes lowered concrete graphs; it does not consume C++
  node-ref types or virtual-node metadata.
