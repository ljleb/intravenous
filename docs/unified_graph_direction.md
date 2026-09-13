# Unified Graph Direction

_Status: working architecture direction, not a final implementation plan._

This note records the direction opened by fast LLVM-based module reload and
graph-composition compilation. It proposes convergence of the lane and DSP graph
models, while retaining the product semantics currently implemented around lanes,
modules, persistence, queries, and specialized UI.

It supersedes the parts of older direction notes that require separate lane and
per-IV-module execution graphs. Those notes remain useful records of existing
behavior and migration constraints.

## The central change

The application should move toward one canonical project graph of ordinary
node instances and connections.

```text
node implementations, cached independently
                +
project graph topology
                |
                v
      generated LLVM graph-composition layer
                |
                v
        executable project graph kernel
```

The old lane/DSP division was justified principally by the cost of changing
DSP topology. If graph composition can be regenerated and compiled quickly,
ordinary rewiring no longer requires a distinct lane execution system.

This does not mean deleting lane code or making every workflow a generic node
editor. It means separating the useful semantics now carried by lanes from the
old execution partition that happens to implement them.

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

- A generated node with stable identity is an addressable project-edge endpoint.
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

Thus a C++ attachment endpoint can be modeled as:

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

## Execution and runtime state

Compiled and realtime remain dataflow semantics, not indicators of different
graph executors. They should be represented explicitly in port/dataflow
semantics and handled during graph-kernel lowering. In particular,
realtime-to-compiled remains a meaningful materialization or recording
operation.

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

The practical starting point is to replace lane nodes in place with ordinary
DSP-style nodes while preserving useful lane-oriented product behavior.

Preserve or reinterpret:

- timeline and lane presentation;
- hierarchy;
- tags, metadata, and query language;
- UI-created graph structure;
- persistence and source navigation;
- state migration; and
- specialized interaction surfaces.

Replace:

- lane execution scheduling;
- separate lane and DSP runtime graphs;
- per-module DSP execution partitions; and
- graph input/output proxy machinery needed only to bridge those systems.

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
- the final compiled/realtime port model and materialization representation;
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
