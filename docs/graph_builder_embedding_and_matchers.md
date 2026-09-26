# GraphBuilder Embedding, Hierarchy, And Project Port Matchers

_Status: direct frozen-graph embedding, explicit handle translation, and the
first recursive `ProjectNodePortMatcher`/`GraphConnections` semantic layer are
implemented. Structured persistence/wire adapters and richer compound-path
coverage remain follow-up work._

This document supplements [graph_builder.md](./historical/graph_builder.md),
[virtual_nodes_channel_aware_graph_direction.md](./historical/virtual_nodes_channel_aware_graph_direction.md),
and [project_graph_application_architecture.md](./project_graph_application_architecture.md).

The implemented live-child/frozen-child importer described here is the current
checkpoint. The planned post-GraphJit/GraphExecutor builder redesign is documented
in [scoped_graph_builder_and_subgraph_closure_direction.md](./scoped_graph_builder_and_subgraph_closure_direction.md).
That later design keeps direct `ConfiguredGraph` import, but replaces newly
constructed live child graphs with zero-copy same-`BuilderSession` nested-scope
finalization. It is intentionally deferred until the runtime work has landed and a
substantial optimization/profiling iteration has been completed.

## Implemented embedding checkpoint

The builder now imports a frozen `ConfiguredGraph` directly; it does not rebuild
a temporary child `GraphBuilder`. Live-child and frozen-child insertion use the
same component importer for node bundles, sample/event connections, detach
state, and virtual-node state. Each frozen placement returns an explicit
`ConfiguredGraphEmbedding` containing the synthetic parent root scope plus
local-to-parent node-bundle/scope and virtual-node translations.

Imported virtual-node records are cloned/remapped one-for-one and receive a
scope-qualified introspection ID. Their source and type identities remain
unchanged. This is deliberate: two embeddings of the same cached configured
graph may contain the same local virtual declaration, but lowering must not
coalesce those declarations back into one root-scope virtual node. Nested
subgraph bundles and tiled child bundles continue to translate through the
node-bundle mapping.

`GraphConnections` now consumes these preserved identities through structured
project matchers. The current path vocabulary has separate selectors for virtual
node identity, ordered virtual members, tiled children, and nested subgraph
scopes; the port matcher separately selects by name/index and optional sample
channel. Matchers are resolved against a frozen view of the complete freshly
embedded root before project connections are applied.

The remaining work in this document is persistence/wire encoding for those
structured matchers plus richer compound-path/adversarial coverage as graph
shapes become more varied.

## One embedding mechanism

> **Current implementation note:** the shared live-child/frozen-child importer in
> this section remains the implemented checkpoint. In the later scoped-builder
> design, only frozen `ConfiguredGraph` placement needs import/remap; a newly
> constructed live child already resides in the parent `BuilderSession` and is
> finalized in place. The lower-level component remapping rules remain reusable.

`ConfiguredGraph` must be directly embeddable into a parent `GraphBuilder`.

Do not rebuild a temporary `GraphBuilder` from a frozen graph merely to call an
existing child-embedding API. Instead, factor the current child graph importer
so both sources use one semantic remapping implementation:

```text
live child GraphBuilder state --\
                                -> child graph importer/remapper -> parent
frozen ConfiguredGraph --------/
```

The importer must preserve every semantic component represented by
`ConfiguredGraph`: node bundles, public interfaces, internal sample/event
connections, detach state, virtual nodes, annotations/source identity, nested
subgraph scope, and tiled-child structure.

## Local handles remain local

Handles inside a cached `NodeInstance` are stable within that frozen configured
graph. They must not be rewritten when the graph is embedded.

Every embedding returns an explicit local-to-parent translation object.
Embedding the same cached instance twice therefore produces two mappings:

```text
cached local bundle 7
    |
    +-- embedding A -> parent bundle 31
    |
    `-- embedding B -> parent bundle 68
```

The mapping should cover every kind of identity needed by recursive project
path resolution, not only a root bundle offset. At minimum this includes:

- graph/scope handles;
- virtual-node handles;
- node-bundle handles;
- concrete child-node handles where separately represented;
- tiled child identity.

The exact representation may use compact offsets where the mapping is truly
contiguous, but callers should consume an explicit translation API rather than
reconstructing offsets ad hoc.

## Preserve hierarchy instead of flattening virtual nodes

Arbitrary recursive child navigation is required by persistent project
connections.

Current behavior that imports child virtual nodes into one flattened parent
identity space is insufficient. A virtual identity named `filter` inside two
different child scopes must remain distinguishable.

`GraphBuilder` and `ConfiguredGraph` therefore need an explicit nested scope
model. The representation can evolve, but it must preserve the invariant:

> Virtual-node identity is resolved relative to a graph/subgraph scope, and
> embedding preserves the complete scope tree.

A conceptual structure is:

```text
root scope
  virtual "voices"
    direct member 0 -> child subgraph scope
      virtual "filter"
    direct member 1 -> child subgraph scope
      virtual "filter"
```

Those two `filter` virtual nodes are different path targets even if their local
source/type identity is the same.

## Direct-member order is stable identity

A virtual node can represent several direct members. Once project persistence
can select a member by index, that order is externally observable stable
identity.

Reload/reconfiguration may replace the concrete implementation represented by a
member, but surviving semantic members must not be arbitrarily reordered.

Builder/configured-graph tests should make this invariant explicit.

## Tiled child structure is first-class

A tiled node bundle represents separately selectable child nodes. The builder
must preserve that distinction structurally rather than forcing later code to
infer it from port channel counts or an ad-hoc side table.

A project path needs two different selectors:

1. **tile child selector** — chooses a tiled child node, preferably through the
   same channel/member identity vocabulary already used by typed tiled
   `NodeRef` selection;
2. **port channel selector** — chooses one concrete channel of a sample port on
   the node(s) matched by the path.

Those operations are not interchangeable.

For example, these have different meaning:

```text
... / tile(left) / port "main" / channel 0
```

and:

```text
... / port "main" / channel 1
```

The first selects a tiled child node before selecting a port. The second stays
on the current matched node and selects a port channel.

## `ProjectNodePortMatcher`

Persistent whole-project connections use `ProjectNodePortMatcher` as the named
concept. Do not introduce a parallel generic port-address term for the
same role.

A matcher has three conceptual fields:

```text
instance id
recursive node-path matcher
port matcher
```

The concrete struct may further separate an optional port-channel selector if
that improves type safety.

The node path is an ordered sequence of structural selectors. It must be able
to express arbitrary alternation between:

- virtual-node selection;
- ordered direct-member selection;
- tiled-child/channel-member selection; and
- descent through nested subgraph scopes.

A serialized debug/UI spelling may resemble:

```text
<uuid>.virtual-a.virtual-b.4.<tile-left>.virtual-c:main:0
```

but project state should use structured fields rather than making one string the
normative parser/ABI.

## Matchers are set-valued

`ProjectNodePortMatcher` does not denote exactly one concrete channel.

Each path step transforms a set of candidate nodes/scopes. A virtual node may
contain several direct members, and later steps apply independently to every
surviving candidate.

Conceptually:

```cpp
MatchSet current = embedding.root_set();
for (auto const& step : matcher.path)
    current = apply_step(current, step);
return resolve_port_channels(current, matcher.port);
```

Resolution therefore returns zero, one, or many concrete sample/event ports or
channels.

Zero matches are a normal dangling/unresolved result, not a reason to delete the
persisted connection.

## Connection semantics

A project sample connection contains:

- an explicit output/source channel type;
- a list of output-side `ProjectNodePortMatcher`s;
- an explicit input/target channel type;
- a list of input-side `ProjectNodePortMatcher`s.

All concrete channels selected by output-side matchers contribute to the source
expression and are summed according to ordinary graph connection semantics.
That source is then applied/broadcast to all concrete channels selected by the
input-side matchers.

Channel type/layout conversion remains explicit graph semantics; matcher
resolution itself should not silently redefine channel type.

Equivalent event-port matcher structures are implemented using event type
identity rather than sample channel type.

## `GraphConnections` transaction

`GraphConnections` receives a root builder only after `NodeInstances` has
finished embedding the complete requested node set for the current transaction.
It also receives the complete external instance-id -> embedding mapping and uses
its own complete desired connection set for the transaction. `ProjectGraph` may
carry a mutation/replay batch into the transaction, but it does not duplicate
that canonical connection intent.

It then:

1. consumes the current requested connection batch for this transaction;
2. resolves every matcher recursively against that complete embedding state;
3. records unresolved diagnostics without deleting desired connections;
4. applies every resolvable cross-node sample/event connection to the same root
   builder;
5. returns one batched result to `ProjectGraph`.

There is no app event from `NodeInstances` directly to `GraphConnections`; both
are invoked once by `ProjectGraph` in the same root-build transaction.

## Matcher/embedding coverage checkpoint

The current frozen-embedding and semantic `GraphConnections` tests cover the
core invariants needed by the in-memory `ProjectNodePortMatcher` model. Before
that matcher model is serialized as persistent/wire state, coverage should
continue to prove at least:

1. a frozen `ConfiguredGraph` embeds through the same importer as a live child;
2. embedding the same frozen graph twice creates distinct parent objects while
   preserving its local identities;
3. the returned translation maps resolve every preserved local object needed by
   project paths;
4. nested virtual-node scopes survive arbitrary recursive embedding;
5. direct-member order is preserved;
6. tiled children remain structurally identifiable after embedding;
7. paths can navigate virtual -> member -> tile -> child scope -> virtual
   repeatedly;
8. port-channel selection remains distinct from tile-child selection.
