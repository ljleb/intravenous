# Unified Graph Direction

_Status: unified-graph direction. The concrete application-module decomposition,
node terminology, caching ownership, recursive project matcher model, and event
procedures are now normative in
[project_graph_application_architecture.md](./project_graph_application_architecture.md),
with whole-project compilation ownership in
[graph_jit_direction.md](./graph_jit_direction.md) and realtime physical
connection planning in
[realtime_port_storage_planning.md](./realtime_port_storage_planning.md).
Where older sections below use `iv module` as the general node abstraction,
describe a separate managed-realization/controller layer, or imply that logical
connections require buffers, the newer documents take precedence._

The immediate application-module cleanup that precedes the replacement executor
is recorded in
[application_module_cleanup_direction.md](./historical/application_module_cleanup_direction.md).
That cleanup deliberately deletes the old lane/task execution modules before
choosing the new isolated execution-module decomposition.

This note records the direction opened by fast LLVM-based module reload and
graph-composition compilation. It proposes convergence of the lane and DSP graph
models, while retaining the product semantics currently implemented around lanes,
modules, persistence, queries, and specialized UI.

It supersedes the parts of older direction notes that require separate lane and
per-IV-module execution graphs. Those notes remain useful records of existing
behavior and migration constraints, but their `Timeline`/lane ownership rules are
not current architecture.

This direction was revalidated after the IV-package/configuration refactor. The
package work changed several mechanisms from the earliest proposal (dynamic
registered construction, greedy iv-module expansion, retained lossless
`ConfiguredGraph`s, and explicit registered primitive provenance), but those
changes make the project-graph convergence simpler rather than invalidating it.

Compiled DSP-port semantics are specified separately and normatively in
[compiled_dsp_nodes.md](./compiled_dsp_nodes.md). This document describes how
that capability fits the unified project graph; it should not restate or replace
the node API, request-planning, or storage rules from that document.

## Settled terminology and ownership

The general registered abstraction is now **node definition**:

- a primitive registered `IV_NODE` definition is a **leaf node definition**;
- a registered `IV_MODULE` graph-producing definition is a **module node definition**.

Their configured reusable results are **node instances**. A cached node instance
may be referenced by several stable project instance ids and embedded several
times, producing distinct runtime storage/state at each placement. Module nodes
remain configuration/project identities rather than execution partitions.

`NodeDefinitions` owns the immutable versioned id-to-provider snapshot.
`NodeInstances` owns recursive configuration and reusable configured instance
caches. `ProjectGraph` owns durable project declarations and orchestrates the root
builder. `GraphConnections` applies project-wide connections after all desired
instances have been embedded. `GraphJit` synchronously compiles the completed
root `ConfiguredGraph`; `GraphExecutor` owns mutable runtime storage, active/
pending compiled generations, execution requests, and safe-boundary activation.

The old proposal for a generic automatically-managed graph-fragment/controller
layer is not part of the current core design. Presentations and optional device
convenience services can issue ordinary batched node/connection requests through
the same project-graph machinery when those features are designed.

## The central change

The application should move toward one canonical project graph of ordinary
node instances and connections.

```text
node implementations, cached independently
                +
project graph topology
                |
                v
      completed root ConfiguredGraph
                |
                v
             GraphJit
      generated/optimized LLVM
                |
                v
          CompiledGraph
                |
                v
          GraphExecutor
```

The old lane/DSP division was justified principally by the cost of changing
DSP topology. If graph composition can be regenerated and compiled quickly,
ordinary rewiring no longer requires a distinct lane execution system.

This does not mean deleting lane-oriented product semantics or making every
workflow a generic node editor. It does mean that the current `Timeline`,
`LaneGraph`, `TimelineExecution`, and graph-input/device proxy lanes are allowed
to disappear. The useful presentation, persistence, query, transport, and data
domain semantics now carried by those objects must be moved to the project graph
or to focused services rather than preserved by keeping a second graph model.

## One canonical project graph

The project graph is the common representation for:

- C++ graph configuration;
- direct graph editing;
- iv-module realization and reconciliation;
- specialized webviews such as timelines and mixers;
- hierarchy and navigation;
- tags, metadata, and queries;
- persistence;
- hot reload and runtime-state migration; and
- LLVM graph lowering.

A node instance is not defined by its presentation, origin, or optimizer
boundary. A node may have been created directly by a user, by C++, by an iv
module, or by a specialized UI and still participate in the same project graph.

The graph is not required to be manually configured. High-level interfaces may
create and manage graph structure, but their realization is still ordinary
project graph state.

## Node types and iv modules

The server needs an explicit node-type registry:

```text
NodeTypeId -> NodeTypeDefinition
```

A definition contains what is necessary to understand, instantiate, reflect,
execute, and migrate a node type: port structure, configuration, runtime state,
lifecycle, its current LLVM implementation, compatibility information, and
presentation metadata.

An **iv module** is the general registered graph-producing,
graph-managing, and optionally UI-providing abstraction. It may:

- declare or provide node types;
- create one node or an arbitrary graph realization;
- reconcile that realization as its definition or project state changes;
- supply a specialized UI or restricted graph editing workflow;
- manipulate live values without recompiling source;
- query and navigate graph state; and
- participate in reification or subsumption where appropriate.

An iv module is therefore not an execution partition. It is an configuration,
management, provenance, and presentation boundary.

A **C++ iv module** is one implementation of an iv module. It is a reusable,
hot-reloadable graph-producing definition configured in C++. An atomic custom
node type is a primitive implementation/schema; it can be provided by an iv
module but does not require a nested execution graph.

## Managed realization and project attachments

An iv-module instance produces a managed realization: nodes, generated edges,
presentation hierarchy, and metadata controlled by that instance's definition.
Generated edges are not directly edited as project-owned edges; changing them
means changing the module definition or reifying the realization.

The criterion for a generated node to receive a user-managed project
connection is stable identity.

- A generated node with stable identity may be matched by persistent project connection state.
- A generated node without stable identity is generated-only; it cannot receive
  a persistent user-managed connection.
- This is not a separate sealing or per-port authorization policy. Normal graph
  validation still enforces port direction, type, channel/layout conversion,
  and multiplicity.

The persistent attachment identity conceptually includes the owning iv-module
instance, the managed node identity, and the port identity. When a reload
reconciles the same identity, its project attachments are retained. If that
identity is absent from a new realization, the attachment becomes dangling;
it is not silently retargeted or deleted. It may resolve again if the matching
identity returns.

### C++ virtual nodes

C++ iv modules already provide this model through virtual nodes. Every
source-configured virtual node has stable identity. A virtual node may represent
one or several concrete members, and the ordering of concrete members under a
given virtual node is itself stable identity.

Thus a C++ persistent port attachment/matcher can be modeled as:

```text
iv-module instance
  / virtual-node identity
  / concrete-member ordering, or the virtual aggregate
  / port identity
```

This permits attachments to a virtual aggregate or to a particular concrete
member without making the underlying execution graph a module boundary.

Other kinds of iv modules use the same rule. They need not resemble C++ source
or virtual nodes; they need only produce stable managed-node identities for the
parts of their realization that users can attach to.

## Connectivity, hierarchy, provenance, and metadata are orthogonal

Graph connectivity is an arbitrary directed graph. Presentation hierarchy is a
separate tree or forest: each node has at most one optional presentation parent.
Neither relation constrains the other.

```text
connectivity:          presentation:
A -> B -> C           Synth
 \-----> D            |- Oscillator
                     |- Modulation
                     |  `- LFO
                     `- Filter
```

An iv-module instance normally controls the presentation subtree of its
managed realization. For C++ iv modules, that hierarchy can follow nested
`GraphBuilder` subgraphs. A cross-boundary project connection does not require
reification; moving a managed node out of its source-controlled hierarchy does.

Provenance is separate again. A node's provenance can record, for example:

```text
iv-module definition type
iv-module instance
generation-local identity/path
generic iv-module metadata
iv-module-specific metadata
```

Tags and other user metadata are also independent. They must not be conflated
with presentation parent, connection topology, management, or runtime state.

## Presentation, lanes, and specialized UI

Lanes are a presentation concept, not an execution primitive. A graph node may
be represented as a lane in one view, while a tree, mixer, timeline, query
result, or custom iv-module UI presents the same graph through a different
interaction model.

The generic tree/wiring UI remains valuable as a direct manipulation surface
and escape hatch. It should not force every user workflow to expose all nodes
and edges. A timeline can create clips and crossfades; a mixer can create and
manage routing; an iv-module webview can offer domain-specific controls. Each
of these realizes or edits the same project graph.

C++ and webviews complement one another:

- C++ is well suited to reusable structural definitions and hot-reloadable
  realization;
- webviews are well suited to live interaction, inspection, navigation, and
  project-local changes.

Eventually, C++ expressions or small fragments may also become an interactive
UI building block. That is exploratory rather than a present architectural
commitment.

## C++ and graph configuration

C++ iv-module configuration and project graph wiring are two representations of
the same structure. The desired transformations are:

```text
C++ iv module -> project-state graph realization
project-state graph -> readable C++ iv module
```

This is semantic round-tripping, not source reconstruction. A loop that
creates eight nodes can become eight explicit reconstructed node declarations;
the resulting code need only recreate the graph meaningfully and readably.

This supports extracting selected graph structure into C++ and later
subsuming compatible ordinary graph structure into a participating iv module.

## Reification and subsumption

**Reify** detaches a managed realization from its iv module while leaving the
graph structure in place. Its formerly generated nodes, edges, metadata, and
hierarchy become ordinary project-owned state. It no longer updates from the
original definition.

**Subsume** is not simply the inverse operation. A participating iv module
interprets selected ordinary graph structure as one of its realizations. It may
need to recognize the graph, infer configuration and role correspondence, ask
the user to resolve ambiguity, validate the result, and only then assume
management. An iv module may opt out of subsumption entirely.

Symmetric graph shapes must not cause arbitrary matching. Tags, provenance
retained from reification, and explicit user choices can help resolve a valid
but ambiguous correspondence.

## Consequences of the implemented IV-package model

The package/configuration implementation now provides several invariants that
should be used directly by the project graph instead of recreating lane-era
adapters:

- a project iv-module instance has a completed, lossless `ConfiguredGraph`;
- registered iv-module calls are greedily expanded during configuration, so no
  recursive registered-module execution boundary remains to be represented at
  project runtime;
- virtual-node identity and ordered direct members survive configuration and are
  the stable addressing seam for project attachments;
- registered primitive leaves retain their stable node-type identity and provider
  provenance independently from build-local compiler keys; and
- source provenance, including virtual-port provenance used by live editing, is
  attached to configured graph structure rather than to lane identity.

Therefore a project connection can address an iv-module instance and one of its
virtual/public ports directly. `GraphInputLanes` does not need a successor
that manufactures one proxy lane per exposed port. The replacement should store
the connection/control state against the stable project node/port matcher and adapt it to
the compatibility executor only for as long as that executor remains.

Public module ports are boundary ports, not implicit project nodes. A
specialized UI may present them as controls or lane-like rows without requiring
extra graph nodes merely for presentation.

## Execution and runtime state

Compiled and realtime are capabilities of ordinary DSP ports in the same graph,
not indicators of different node families or graph executors. The normative
contract is in [compiled_dsp_nodes.md](./compiled_dsp_nodes.md). In summary:

- sample/event kind and realtime/compiled access are orthogonal declaration axes;
- a declaration chooses either bounded realtime timing (`RealtimeInputConfig` /
  `RealtimeOutputConfig`) or compiled random access (`CompiledPortConfig`);
- compiled ports still expose the ordinary current-block typed wrapper during
  `tick()` / `tick_block()`, so additivity belongs to the accessor API rather than
  to a combined realtime+compiled config;
- a compiled sample output can be requested at arbitrary global sample positions,
  while a compiled event output can be queried over arbitrary global intervals;
- a compiled input extends the corresponding ordinary realtime sample/event
  access rather than replacing it with a separate resource API;
- compiled sample requests may use sparse sampled grids, while compiled event
  requests preserve every event in the requested interval;
- compiled access does not imply persistent materialization, buffering, or
  caching;
- sequential `tick_block()` and arbitrary `access_block()` are distinct execution
  modes, with `access_block()` restricted to compiled ports and `CompiledState`;
- a compiled query is planned globally: demands propagate in reverse topological
  order, request sets are unioned/coalesced, and evaluation then runs forward;
- temporary representation/materialization is chosen only after planning; and
- the initial graph has no implicit realtime-to-compiled edge; any recording is
  an explicit node with a realtime input and compiled output, rather than a
  general computed-output cache policy.

The initial compiled-data implementation deliberately has no persistent cache of
deterministic computed outputs. `CompiledState` is node-managed arbitrary-access
state and is distinct from any future framework cache. Old `TimelineExecution`
compiled caches, invalidation spans, explicit recording-lane requirements, and
prepared-resource input APIs are therefore migration history, not replacement
architecture.

Realtime sample/event connections follow the same storage-independent principle.
`ConfiguredGraph` records logical connection semantics only. The whole-project
compiler derives history/latency/event-window correctness requirements, chooses
physical connection implementations with a pure testable planner, performs
transient liveness/scratch reuse, and only then emits LLVM. Realtime event
outputs must have finite compiler-known production windows; compiled event access
remains arbitrary-range. See
[realtime_port_storage_planning.md](./realtime_port_storage_planning.md).

`GraphJit` owns that synchronous whole-project compiler/ORC domain. It compiles
one coherent configured/provider generation and returns an immutable
`CompiledGraph`; `GraphExecutor` owns live `NodeStorage` and activates successors
only at legal audio-pass boundaries.

The executable kernel is replaceable. Logical node state survives when a
stable node correspondence and compatible state layout survive:

- retained nodes migrate compatible state;
- new nodes initialize it;
- removed nodes release it; and
- compatible node-type changes can apply module-defined migration.

UI boundaries—lanes, groups, iv modules, and specialized views—must not become
compiler optimization boundaries. A topology edit normally invalidates only
the graph-composition/kernel layer, while a source edit invalidates affected
node implementations and the kernels that use them.

## Migration posture

Do not require a feature-preserving, class-by-class conversion of lane nodes into
new graph objects. The lane subsystem is sufficiently entangled that preserving
its implementation while moving ownership can retain obsolete constraints. Use
the old code and tests as a requirements inventory, establish the replacement
semantic capabilities and durable project ownership, then allow the obsolete lane
execution architecture to be deleted as an architectural cut. Product features
that disappear in that cut can be reintroduced afterward using ordinary DSP nodes,
general iv modules, focused services, or project/UI state as appropriate.

The capabilities that must be designed independently of the old lane
implementation are:

1. **Compiled DSP ports.** Implement the semantics in
   [compiled_dsp_nodes.md](./compiled_dsp_nodes.md): compiled access is
   orthogonal to sample/event kind, so both compiled sample and compiled event
   ports remain first-class. Use global batched demand planning, cacheless
   computed access initially, kind-appropriate sample/event request semantics,
   and explicit realtime-to-compiled recording nodes where a graph needs that
   transition. Do not migrate `TimelineExecution` compiled caches or invalidation
   machinery into this model.
2. **General iv modules.** Complete the separately planned abstraction by which an
   iv module need not be backed by a C++ IV package, may own/manage a project
   subgraph, and may provide a custom UI. The exact API remains follow-up design
   work; lane deletion should not force that API to imitate lane types.
3. **Canonical project ownership.** Provide enough project-owned identity,
   connections, dangling matcher state, hierarchy/metadata, controls, and
   persistence that deleting `Timeline` does not delete the project's topology or
   user state. C++ iv-module instances can continue to use their retained
   `ConfiguredGraph` realization and stable virtual/member/port identities.
4. **Focused host services.** Retain or extract device callback/buffering and
   transport/playback state only where those are independently useful product
   services. They should not remain lane semantics merely to keep the old graph
   alive.

Once those ownership/capability boundaries exist, `Timeline`, `LaneGraph`,
`TimelineExecution`, lane task production, graph-input/device proxy lanes, and
compiled-lane execution machinery may be removed together. It is acceptable for
specialized features such as beat-trigger generation, automation editors, audio
file capture, or lane visualizations to be temporarily absent on the migration
branch and then return in forms native to the remaining system. Git and the old
tests preserve the former implementation; they do not require a one-to-one object
migration.

A compatibility adapter from the canonical project graph to per-instance
`RuntimeGraphRoot`/`TasksRunner` execution is optional migration scaffolding, not
an architectural requirement. If retaining application functionality during the
delete is useful, direct cross-instance project edges may temporarily become task
dependencies plus block/event transfer. If a destructive cut is simpler, the
project model should not be distorted merely to preserve that adapter. The future
whole-project generated kernel consumes the same canonical project graph either
way.

Preserve or reinterpret:

- lane/timeline presentation where it remains useful;
- hierarchy;
- tags, metadata, and query language;
- UI-created graph structure;
- persistent project connections and dangling matcher behavior;
- source navigation and live-edit controls;
- transport semantics that remain part of the product;
- state migration; and
- specialized interaction surfaces.

Replace:

- `Timeline` as the canonical graph owner;
- lane execution scheduling and compiled/realtime executor partitioning;
- separate lane and DSP runtime graphs;
- graph input/output proxy lanes; and
- eventually, per-module DSP execution partitions.

The existing codebase and tests remain a behavioral specification and migration
inventory. New architecture should preserve their product semantics where they
remain valuable, not their obsolete execution boundaries.

## Open design work

The following are intentionally unresolved:

- the exact server API for node-type, iv-module-definition, and iv-module-
  instance registration;
- which realization data is persisted and which is regenerated;
- state-compatibility and migration contracts;
- readable graph-to-C++ generation;
- reification provenance and subsumption correspondence APIs;
- interactions between user-created and iv-module-managed hierarchy;
- C++ expression support in ordinary webviews;
- low-level compiled-port API/ABI choices intentionally left open by
  `compiled_dsp_nodes.md` (including exact sample-request endpoint/index mapping,
  compiled-event range APIs and ordering, request-set representation/coalescing,
  and concrete planner/runtime data structures);
- kernel invalidation, caching, inlining, and state layout; and
- the most useful generic and specialized graph-editing surfaces.

Convergence should come from small prototypes and use: observe, implement,
exercise, identify the invariant, retain or discard the experiment, and update
this direction.

## Decisions currently treated as strong

1. The project moves toward one canonical graph of ordinary node instances and
   connections.
2. Lanes and DSP nodes converge on one node model; lanes remain presentation.
3. LLVM graph composition produces the executable kernel; UI concepts are not
   execution or optimization partitions.
4. `iv module` is the name for the general graph-producing, graph-managing,
   optionally UI-providing abstraction. C++ iv modules are one implementation.
5. Node types require explicit server registration.
6. An iv-module realization is managed project graph structure, not a separate
   runtime graph.
7. Stable managed-node identity is the sole criterion for whether generated
   nodes can receive persistent user-managed project connections.
8. Every C++ virtual node has stable identity, and concrete-member ordering
   within that virtual node has stable identity.
9. Generated edges are managed; project attachments are ordinary persistent
   graph edges and survive reconciliation by endpoint identity.
10. Connectivity, presentation hierarchy, provenance, tags, and runtime state
    are independent relations.
11. Reification is mechanical detachment; subsumption is interpretive and may
    require user disambiguation.
12. Runtime-state migration remains essential, while the old lane scheduler and
    module execution partitions do not.
13. Compiled data is an ordinary DSP-port capability, not a parallel lane/node
    graph or a storage class.
14. Compiled capability does not imply persistent materialization or caching;
    storage is chosen from actual query/consumer demand.
15. Arbitrary compiled access is globally demand-planned before execution, with
    reverse requirement propagation followed by forward evaluation.
16. The initial framework does not persistently cache deterministic compiled
    outputs. Explicit realtime-to-compiled nodes may own retained source data;
    that is a separate concept from such a cache.
