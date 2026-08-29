# Event Propagation Tree Constraint

This note specifies the tree constraint on event propagation between
application modules: what it is, why it is worth enforcing, what it costs, and
what falls out of it. It complements
`typed_linker_bridge_migration.md` and
`application_modules_event_architecture.md`, which define the module/bridge
machinery this builds on.

## Terminology

Three axes describe an interaction between two units of the application:

1. **Control flow** — calling A leads to B being called, or the reverse.
2. **Data flow** — information passes from A to B, the reverse, or both.
3. **Dependency flow** — which unit knows about the other.

The bridged-module architecture deliberately separates the first axis from the
third. Bridges make dependency flow static and explicit (both endpoints are
named in one declaration), while control flow is carried dynamically by events.
Data flow rides along the same edges as control flow.

A **control source** is a unit where control originates without a prior cause
inside the application. Sources are not necessarily modules. Examples:

- the main function;
- a client message arriving in the JSON-RPC transport;
- a project file (`iv_project.json`) being loaded;
- a module file (`iv_module.json`) changing on disk;
- the audio callback boundary.

The architecture's constraint is that each such source induces a well-formed
propagation over the application modules.

## The constraint

**From any single control source, the module-level control flow graph reachable
through event propagation is a strict tree with unique nodes — never a DAG, and
never a graph with re-entrancy.**

Nodes are application modules. Edges are event propagations: a module raises an
event, a subscriber member of another module on a shared bridge receives it,
and that member may in turn raise further events. A bridge itself is not an
edge in this graph; it is the static *declaration* that an edge may exist
between its two participants.

An implied property, and a useful self-check: **for any single event
connection, the propagation subgraph reachable from that connection is also a
tree.** If a subtree of one event forms a DAG, so does some whole-source
propagation, and vice versa.

## Why a tree and not a DAG or a general graph

Event propagation as a general graph is hard to reason about: a module can be
entered from several points, and it is no longer easy to track what causes
which state change in that module.

A DAG is better but still problematic. Consider:

```
A -> B
A -> C
B -> D
C -> D
```

`D` is reached twice for the same original reason. Consequences:

- `D` may observe its own state in a partially invalid configuration after the
  first arrival;
- or `D` must do extra work to remain valid at all times, even though the
  intermediate states are never observable;
- or `D`'s reaction runs twice when once would do, doubling side effects.

A closely related failure mode with the same conclusion: a module reacts to a
batch of changes by raising one event per change in a loop, instead of one
batched event. Downstream modules re-propagate once per element and pay the
same re-entrancy or wasted-invalidation cost.

A strict tree eliminates these by construction: each module in a propagation
has exactly one incoming path, so every state change it performs has a unique
`(source, cause)` explanation.

## Causality, not topology

The constraint is a property of **causes**, not of the static bridge graph.

Two modules can statically form a cycle through their shared bridge and still
satisfy the constraint, as long as the two directions are only ever triggered
by different original sources. Conversely, a statically acyclic bridge graph
can still violate the constraint at runtime if one propagation loops.

The precise rule is therefore:

- **Branching is fine** when a module raises one event with multiple
  subscribers. Both branches carry the *same* cause and are siblings in the
  tree.
- **Re-entrancy is the violation**: one module receiving the *same cause* more
  than once. This happens when two different paths from one source converge on
  the same module, or when a module raises an event in a loop.

A useful mental model: each event propagation carries an implicit cause token.
The tree constraint says: for each source and each cause, no module processes
that cause twice.

### Raising multiple events from one member

A module member may raise as many events as it needs. Raising several events is
not a violation by itself — each raised event spawns a subtree, and the
constraint only requires that those subtrees not re-enter a module already on
the current path, and not re-enter each other's modules for the same cause.

Forbidding multiple raises per member would be far too constraining: a member
often legitimately publishes several distinct notifications, or a
request/response pair plus a state-change notification.

### Why the same event can be raised for different reasons

Event identity does not determine cause. The same event may be raised from
several members, or from several call sites in one member, each representing a
distinct reason. The propagation trees of two different event sources can
legitimately share a subtree — that is sharing by *different causes*, which is
allowed.

This is why the tree property cannot be decided at compile time from static
facts alone: static reachability can identify *candidate* collisions, but only
reasoning about causes can decide whether a collision is real. Static analysis
over-approximates and will flag some propagations that cannot actually happen
at runtime; the architecture accepts this and treats flagged collisions as
review items rather than proof of a bug.

## Source modules

A source module is a module designated as an origin of control: all of its
externally visible events are initially triggered by it, never by a
propagation arriving at it.

Designating modules rather than individual events is the workable option: it
keeps the analysis well-defined, since a source's *whole* event surface is
root-level. The restriction that comes with designation is deliberate:

> **A source module does not subscribe to events.** It only raises.

If a source module also subscribed, it would have two kinds of causes — its own
originations and arriving propagations — and the tree rooted at "its own
origination" would no longer be a pure tree from a single source. Where a
module needs both roles, split it: the subscribing half delegates internally to
the raising half, and only the raising half is declared a source.

Typical source modules in this application:

- `SocketRpcServer` (client messages);
- `ProjectPersistence` (project file load/save);
- the module watcher's reload service (filesystem changes);
- the audio device boundary.

## Cost and what it buys

**Buys:**

- every state change in a module is explainable by a unique path from one
  source and one cause;
- no double-propagation waste, no partially-invalid intermediate states from
  re-entrancy;
- batched events are the natural idiom: one cause, one event, one subtree;
- debugging "what caused this" is reading one path, not exploring a graph.

**Costs:**

- some module interactions must be reshaped: convergent flows (`B→D`, `C→D`)
  must be reorganized — batched at the raise site, merged into one event, or
  split so the convergence happens in a *subscriber* that fans out rather than
  a module that is entered twice;
- modules that legitimately need both source and subscriber roles must be
  split;
- static analysis of the constraint over-approximates: some flagged collisions
  are impossible at runtime and need explicit reasoning (or an explicit
  cause-distinguishing annotation) to dismiss.

The constraint is a design discipline first and a checkable property second.
Most of its value is in shaping how modules and events are factored; the
validation exists to keep that shape from eroding.

## Enforcement

The constraint is whole-program and cause-dependent, so it cannot be fully
checked at compile time. It is enforced in tiers:

### Local, compile time

Cheap checks that fail where the mistake is made:

- bridge subscriptions statically name their participant modules, so a bridge
  connecting a declared source module *as a subscriber target* is a
  compile-time error;
- a module raising an event that its own module subscribes to (an immediate
  one-hop self-loop) can be flagged at the raise site.

### Global, post build

The remaining invariants are whole-program properties over the merged set of
bridges, raise sites, and declared sources:

1. every event has at least one raise site or one declared source;
2. for each declared source module, the module-level propagation reachable
   through events and bridge subscriptions contains no repeated module on any
   single path (tree per source);
3. per event connection, the reachable subgraph is likewise a tree.

The check runs as a post-build validation step and reads a metadata description
of the program's bridges, raise sites, and declared sources. On violation it
reports the offending structure with enough context to act on — the colliding
module, the two or more module paths that reach it, and the file/line of the
raise sites in each contributing module.

Diagnostics are grouped by collision:

```
error: module 'Timeline' is reached twice from source 'SocketRpcServer'
  path 1: SocketRpcServer -> GraphInputLanes -> Timeline
          raise: graph_input_lanes.cpp:412  (iv_runtime_..._batch_requested)
  path 2: SocketRpcServer -> AudioDeviceLanes -> Timeline
          raise: audio_device_lanes.cpp:88   (iv_runtime_..._resumed)
```

A collision is either a real design issue (batch the raises, split an event,
split a module) or an over-approximation (the two paths are mutually exclusive
at runtime), in which case the raise sites must be annotated to distinguish
their causes so the analysis can separate them. Cause-distinguishing
annotations are the only suppression mechanism; they are deliberate, visible,
and keep the default strict.

## Implementation notes

See the implementation strategies discussed alongside this document:

- bridge and raise-site macros can emit constexpr metadata records into
  dedicated linker-set sections, merged across translation units automatically
  at link time;
- reflection provides module, class, and member names at compile time, so no
  strings need to be hand-maintained;
- the post-build validator reads those sections from the linked binary,
  reconstructs the module graph, and runs the per-source tree check;
- the validator and its CMake integration live in a self-contained directory
  imported by the parent `CMakeLists.txt`, so the whole check can be copied
  into another project;
- metadata sections carry no runtime cost and can be stripped from release
  binaries after validation.
