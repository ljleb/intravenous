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

Indexed DSP-port semantics are specified separately and normatively in
[indexed_dsp_nodes.md](./indexed_dsp_nodes.md). This document describes how
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

Indexed and realtime are capabilities of ordinary DSP ports in the same graph,
not indicators of different node families or graph executors. The normative
contract is in [indexed_dsp_nodes.md](./indexed_dsp_nodes.md). In summary:

- sample/event kind and realtime/indexed access are orthogonal declaration axes;
- every indexed output publishes finite `IndexedCoverage`, a canonical union of
  disjoint half-open regions; there is no separate indexed extent abstraction;
- indexed sample/event access is legal only inside coverage, and an indexed
  input exposes the union of the mapped coverages of its connected outputs to
  both `tick_block()` and `tock_region_batch()`;
- canonically aligned cache pages are a physical materialization unit only: a
  page is wholly valid/invalid for exactly `page_interval & coverage`, so page
  boundaries never force node code to process outside actual coverage;
- sparse UI/logical demand selects touched pages. Valid pages are reused; an
  invalid touched page is promoted to its complete covered page domain before
  reverse dependency propagation/tock processing;
- arbitrary mutations may schedule forward processing even with zero changed
  indexed inputs, may update output coverage, and may report exact changed
  output regions without forcing evaluation; newly created semantic nodes use
  this same forward path to establish their initial output coverage;
- added/removed coverage and exact changed regions propagate forward through the
  indexed subgraph; cache invalidation may be page-granular locally but does not
  widen the semantic change sent downstream;
- reverse requirements are clipped to coverage; valid upstream pages terminate
  traversal, while invalid upstream pages expand to their full covered page
  domains before reverse propagation continues;
- changing the connection set of an indexed input conservatively marks that whole
  logical input changed over `old_input_coverage | new_input_coverage`, after
  which normal forward propagation determines downstream consequences;
- `tock_region_batch()` receives only the selected covered page domains, never
  uncovered physical-page portions, already-valid pages, or blind sparse caller
  positions;
- forward and reverse region propagation are opposite directional dependency
  queries, not inverse mappings;
- fixed node/indexed persistent state remains in canonical `NodeStorage`, while
  dynamically growing indexed caches live in an executor-owned stable cache
  store with per-generation endpoint bindings; outputs without stable project
  identity may use generation-local cache state, and request-sized transaction
  storage remains executor-owned;
- indexed results are semantic-versioned. Realtime continues using one immutable
  previously published indexed snapshot while edits/tocks build a newer
  candidate off the hot path; and
- a live-required candidate publishes atomically only at a whole-live-graph block
  boundary after every indexed page the live graph may read is ready. The audio
  thread never waits for or triggers indexed tock processing; and
- JIT generation replacement alone is not an indexed invalidation event: stable
  virtual-node/member output identities rebind new executable generations to the
  same executor-owned cache entries without copying page payloads, while only
  actual state/connection/coverage/semantic changes invalidate candidate data.

The graph has no implicit realtime-to-indexed edge. Recorder/source nodes own any
transition from realtime mutation to changed indexed output regions and report
bounded change notifications for executor-side propagation after the live pass.

Legacy `compiled` port/callback/state identifiers may remain temporarily in the
implementation while this terminology migrates. `CompiledGraph` keeps its name
because it is the actual GraphJit artifact, not an indexed-data object.

Realtime sample/event connections follow the same storage-independent principle.
`ConfiguredGraph` records logical connection semantics only. The whole-project
compiler derives history/latency/event-window correctness requirements, chooses
physical connection implementations with a pure testable planner, performs
transient liveness/scratch reuse, and only then emits LLVM. Realtime event
outputs must have finite compiler-known production windows; indexed event access
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

1. **Indexed DSP ports.** Implement the semantics in
   [indexed_dsp_nodes.md](./indexed_dsp_nodes.md): indexed access is orthogonal
   to sample/event kind; exact changed regions propagate forward without eager
   evaluation; retained cache validity is whole-page for exact covered page
   domains; sparse logical demand selects invalid pages whose covered domains
   propagate in reverse and execute through `tock_region_batch()`; and realtime
   observes only complete published indexed snapshots. Realtime-to-indexed
   transitions remain explicit recorder/source semantics rather than implicit
   graph edges.
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
- lane execution scheduling and legacy indexed/realtime executor partitioning;
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
- low-level indexed-port API/ABI/tuning choices intentionally left open by
  `indexed_dsp_nodes.md` (including exact request endpoint/index ABI, concrete
  segmented-event iterator types, cache page width, payload/arena layout,
  automatic-cache heuristics, live-read-requirement traits, and concrete
  mutation/notification data structures);
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
13. Indexed data is an ordinary DSP-port capability, not a parallel lane/node
    graph or a storage class.
14. Indexed outputs publish canonical sparse coverage; node code never requests
    indexed values outside that coverage.
15. Indexed cache validity is page-granular without widening semantic coverage:
    one canonical page is valid/invalid for exactly `page_interval & coverage`;
    cache retention is a separate `automatic` / `always` / `never` policy.
16. Exact changed indexed regions and coverage changes propagate forward without
    eager evaluation. Later access selects touched pages, reuses valid pages,
    promotes invalid pages to their exact covered page domains, propagates those
    domains in reverse, and evaluates them with `tock_region_batch()`.
17. Explicit realtime-to-indexed recorder/source nodes may mutate authoritative
    indexed source data and seed changed output regions/coverage without forcing
    downstream recomputation. Live DSP continues using the prior immutable
    published indexed snapshot until a complete candidate publishes at a
    whole-live-graph block boundary.
