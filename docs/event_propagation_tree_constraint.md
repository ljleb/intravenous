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
third. Bridges make dependency flow static and explicit (both participants are
named in one declaration), while control flow is carried dynamically by events.
Data flow rides along the same edges as control flow.

A **control source** is a unit where control originates without a prior cause
inside the application. Sources are not necessarily modules. Examples:

- the main function;
- a client message arriving in the JSON-RPC transport;
- a project file (`iv_project.json`) being loaded;
- an IV-source manifest (`iv_package.json`) changing on disk;
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

The tree constraint applies to **application-module event propagation**, not to
internal actor handoffs inside one module. For example, `GraphExecutor` owns internal
`RealtimeExecutor` and `BackgroundExecutor` actors that exchange provisioned block
chains, generation-cutover pointers, and persisted-state pointers in both directions.
Those handoffs are ordinary internal runtime synchronization and do not add vertices or
edges to the app-module propagation tree.

App modules themselves may also have bidirectional static bridge relationships. What is
forbidden is re-entering the same app module twice for one propagated cause. The
constraint is therefore on each dynamic propagation graph, not on the undirected/static
module relationship graph.

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

## Source invocations, not permanently source-only modules

The tree is rooted at a **control-source invocation**, not at a permanent class
of source-only app modules.

For example, one incoming client message can start a tree at `SocketRpcServer`,
while a later independent presentation-update cause may reach `SocketRpcServer`
as a notification sink. Those are different causes and therefore different
trees.

The hard rule is local to one cause:

> An application module may appear at most once in the propagation tree rooted
> at one source invocation.

A module may therefore be a root in one procedure and a child in another. What
is forbidden is a procedure that starts at a module, takes some path through
other modules, and then re-enters that same module before the original cause has
unwound.

Typical external source invocations include:

- a client message arriving in `SocketRpcServer`;
- project-file replay initiated by `ProjectPersistence`;
- Linux package filesystem/discovery activity entering `PackageWatcher`;
- hardware/audio callbacks entering the device domain;
- future presentation/user-interface events.

When an asynchronous operation is scheduled by one cause and completes later,
its completion starts a new source invocation/tree. This is often the correct
way to avoid re-entry through long-running compile/reload work.

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
- causality is a runtime property: only the paths a build actually exercises
  are validated, so the runtime check is a guarantee over executed behavior,
  not a static proof over all possible behavior.

The constraint is a design discipline first and a checkable property second.
Most of its value is in shaping how modules and events are factored; the
validation exists to keep that shape from eroding.

## Enforcement

The tree property is causal and whole-program, so a complete static check is
not achievable: "can propagation from `E` ever reach `M` twice?" reduces to
arbitrary program reachability and control flow. A static validator must
either miss violations or report impossible ones, and doing it well requires
per-translation-unit call-graph metadata plus a post-build whole-program
merge — a large and noisy mechanism.

The architecture therefore validates the property **at runtime, in debug
builds**, where the check observes actual causes instead of approximating
them. Static checks remain only for the cheap, structurally obvious cases.

### Local, compile time

Cheap checks that fail where the mistake is made, with no semantic analysis:

- bridge subscriptions statically name their participant modules, so a bridge
  connecting a declared source module *as a subscriber target* is a
  compile-time error;
- a module raising an event that its own module subscribes to (an immediate
  one-hop self-loop) can be flagged at the raise site.

These catch the trivial violations early; everything else is a runtime
concern.

### Runtime propagation context

Debug builds thread a **propagation context** through event invocations. The
context is created when an **event source entry** opens, with
`IV_EVENT_SOURCE(SourceModule, name) { ... }`, and inherited by every event
raised inside that block:

```cpp
struct PropagationContext {
    SourceFrame source;            // source module, debugging name, location
    std::unordered_set<ModuleId> visited_modules;
    // causal trace entries
};
```

The source module is recorded as visited when the entry opens, so a
propagation that comes back to its own source is a re-entry like any other.
Each subscriber dispatch marks its module visited; raising a further event
inside a handler reuses the same context rather than creating a new one. The
invariant enforced dynamically:

> **Within one causal propagation, a module may be reached at most once.**

Tracking the *set of modules visited during the whole propagation* — not the
currently active call stack — is what makes sibling convergence visible.
Synchronous sequential dispatch (`root -> B -> D -> back -> C -> D`) would
leave `D` off the stack by the time `C` raises into it; the visited set still
contains it, so the violation is caught.

The context travels via the invocation mechanism: `IV_INVOKE_LINKER_EVENT` and
`IV_INVOKE_SINGLETON_EVENT` inherit the context of the enclosing source entry
around subscriber dispatch, and a raise outside any source entry is an error.
A thread-local current context is sufficient for synchronous propagation and
adds no cost in release builds, where all of this compiles out.

Because the check observes the branch that actually executed, conditions are
handled naturally: `if (foo) raise(A); else raise(B);` never produces a false
collision.

### Diagnostics

Recording the causal edge that led to each visit makes the failure
self-explanatory — not "module entered twice" but the two concrete paths:

```
invalid event propagation: module ProjectGraph reached twice

root:
    source_changed

first path:
    source_changed
      -> PackageDefinitions::handle_source_changed
      -> definitions_changed
      -> ProjectGraph::handle_definitions_changed

second path:
    source_changed
      -> NodeDefinitions::handle_source_changed
      -> nodes_changed
      -> ProjectGraph::handle_nodes_changed
```

Each path is exactly the subscriber -> call chain -> raise sequence that
produced the second visit, which is the information needed to fix the design
(batch at a raise site, split an event, or split a module).

### Asynchronous propagation

The main semantic decision the runtime check forces: when an event is queued
and handled later (task runner, deferred work), is it a **new propagation
root**, or a **continuation** of the causal propagation that queued it?

- Treating queued work as a new root is the default; it is simple and cannot
  produce false positives, at the cost of not tracking causality across
  asynchronous boundaries.
- Treating it as a continuation is more precise and preserves the invariant
  across deferrals, but requires the propagation token to travel with the
  queued work.

Either choice must be explicit. What is not allowed is silently dropping the
context at an asynchronous boundary while still claiming full propagation
tracking.

### Static validation's reduced role

A static validator can still catch provable structural errors (trivial cycles,
direct `E -> handler -> F` convergence visible in the bridge table), but it
deliberately remains cheap and conservative:

- **static**: "I can prove this topology is bad";
- **runtime**: "this concrete execution demonstrated that the topology is bad".

The runtime side is the authority for the tree constraint; the static side is
an optional fast-fail filter, not a prerequisite.

## Runtime mechanism design

The runtime check is implemented implicitly and thread-locally: event
signatures do not change, and modules never pass provenance objects manually.
Provenance is an execution concern of the event mechanism, not part of module
APIs.

### Public API

_Accepted design change, 2026-11-12: event sources are entered explicitly with
a scope. The previous single-raise source primitive is preserved in
[event_source_marking_before_event_source_scopes_2026-11-12.md](./historical/event_source_marking_before_event_source_scopes_2026-11-12.md)._

```text
IV_EVENT_SOURCE(SourceModule[, name]) { ... }
    - enters one event source for the block that follows
    - must not be entered while another event source entry is active on this thread
    - SourceModule is where control originates; it is recorded as visited, so
      nothing raised inside the block may enter it again
    - name is an optional debugging name, printed next to the source module in
      collision reports (for example the JSON-RPC method of one client message)
    - everything raised inside the block is one propagation tree
    - the context is destroyed when the block exits, normally or by exception

IV_INVOKE_LINKER_EVENT(event_name, ...)
IV_INVOKE_SINGLETON_EVENT(event_name, ...)
    - raise an event as a consequence of the current source entry
    - raising one outside an event source entry is an error
```

One source entry is one tree. A source block that raises several events
raises them as siblings of that tree, never as several trees. A function may
enter the same source more than once only for distinct causes, as
`PackageWatcherService` does when it reports `rebuildStarted` before entering
the refresh it announces.

The subscriber function type is unchanged: subscribers remain plain
`void(Args...)`. In particular the event context is not injected as a first
parameter; that would infect every module API with an execution concern.

`IV_DECLARE_LINKER_EVENT`, `IV_DEFINE_LINKER_EVENT`, and the linker-section
representation are unchanged. The section still holds subscriber function
pointers; the templated bridge function already knows the concrete subscriber
type at exactly the point where module identity is needed, so there is no
reason to widen the section's ABI with metadata.

### Thread-local state

Every event is raised inside a source entry, so one thread-local pointer to the
current propagation context is enough: it is null exactly when no source entry
is active on the thread. A raise with a null context fails, and a source entry
opened with a non-null context fails. A source therefore cannot nest under an
event either: the event's own source entry is still active.

### Invocation mechanics

The macro bodies move into internal helpers so RAII can maintain state and
exceptions cannot corrupt it:

```text
IV_EVENT_SOURCE:
    assert no active propagation
    create propagation context; record the source module as visited
    run the block
    destroy context
IV_INVOKE_LINKER_EVENT / IV_INVOKE_SINGLETON_EVENT:
    assert an active propagation
    push event frame -> dispatch subscribers -> pop
```

Subscriber wrappers record module entry. The bridge subscriber template
(`iv_bridge_subscriber<Bridge, Member>::invoke`) knows the concrete subscriber
class, which is the module identity for the visited-set check; the dispatch
becomes:

```text
check/record module visit -> push module frame -> member body -> pop
```

### Module identity

Module identity is type identity: the subscriber's participant class. This is
consistent with the bridge model, where a participant is a concrete type and
bridges connect types, not instances. If two semantically distinct module
instances of the same C++ type ever need distinguishing, identity would switch
to something instance-based — but type identity matches the architecture
today.

### Module transactions

_Accepted design change, 2026-11-12._

Application-event propagation and transactional computation are different.
The tree constraint governs the first: two event paths may not reach one
module for one cause, because the module would perform redundant work or
observe incomplete intermediate state. A module may still perform several
computational steps for one cause when those steps form one transaction whose
intermediate state is not published as independent application changes.

A module transaction is opened by one event delivery. The handler keeps its
entry with `EventModuleVisit::current<Module>()` and returns a transaction
object on the request record. Each later operation runs inside
`EventModuleVisitScope(visit)`: it continues the opening entry instead of
entering the module again, and whatever it raises is still checked, as raised
by that module. Using the transaction outside the propagation that opened it
is an error.

A transaction:

- works on transaction-local data; its intermediate steps change no durable
  state and raise nothing;
- commits its accepted result once and raises the consolidated changes once;
- exposes narrowly defined operations and does not re-raise the events that
  opened it; and
- does not hold module mutexes across its lifetime unless its ownership and
  synchronization design explicitly justify it.

Node-message interactions use this: `ProjectGraph` opens one
`NodeInstancesInteraction` and one `GraphConnectionsInteraction` for an
interaction, and runs every replacement preview and the final replacement set
through them. See
[Node message interaction](./event_flows/node_message_interaction.md).

### Provenance traces

Detection needs only `visited_modules.contains(module)`, but the diagnostic
needs the path. The live path is tracked as alternating frames:

```text
[event A] [module B] [event C] [module D] ...
```

On a violation, the first visit's retained path snapshot plus the current path
yield a full two-path report:

```
event propagation re-entered module ProjectGraph

source:
    project mutation source

first entry:
    source event
      -> ProjectGraph::handle_first_change
      -> derived event A
      -> ConsumerA::handle_change

second entry:
    source event
      -> ConsumerB::handle_change
      -> derived event B
      -> ProjectGraph::handle_second_change
```

The runtime checker therefore doubles as an architecture debugger: every
failure reports the exact subscriber -> raise chains that collided.

### Every raise has a source

Every control source is annotated (2026-11-12), and a raise outside a source
entry is an error, so a new control source cannot bypass the invariant:

```cpp
IV_EVENT_SOURCE(Source, "cause") { IV_INVOKE_LINKER_EVENT(E, ...); }  // new cause
IV_INVOKE_LINKER_EVENT(E, ...)  // consequence of the current cause
```

The current source entries are:

| Source module | Entry | Opened in |
| --- | --- | --- |
| `SocketRpcServer` | one client message, named by its JSON-RPC method | the request command handler |
| `SocketRpcServer` | one client connection cleanup | the client cleanup command |
| `PackageWatcherService` | one package refresh; `rebuildStarted`; a discovery or service failure report | its worker |
| `ProjectPersistence` | one project-file replay | `ProjectPersistence::load` |
| `ProjectAutosave` | one autosave attempt, including its failure report | the autosave service thread |
| `ProjectGraph` | one finished control compile | its control-compile worker |

### Asynchronous boundaries

The nested-source rule is unconditional in debug builds: an event source entry
opened inside another fails, because "source" means *no preceding
application-level event cause on this thread*, and an active entry
demonstrably contradicts that.

The sanctioned escape is a deliberately defined cause-breaking boundary. Work
scheduled for later (task runner, deferred queue, worker thread) executes
outside any source entry, so it enters a source of its own: the completion is
genuinely a new cause. `ProjectGraph`'s control-compile completion and
`ProjectAutosave`'s save are such entries. If queued work must instead retain
the original cause, that requires an explicit movable propagation token; that
mechanism should not be built until a concrete requirement exists.

## Implementation notes

- `linker_event.h`: thread-local propagation context, RAII scopes around
  `IV_EVENT_SOURCE` and both invocation macros, and `EventModuleVisit` /
  `EventModuleVisitScope` for module transactions;
- `bridge.h`: module-entry guard in `iv_bridge_subscriber::invoke`, which
  already holds the participant type for the visited-set check;
- no changes to `IV_DECLARE_LINKER_EVENT` / `IV_DEFINE_LINKER_EVENT`, the
  subscriber function type, or the linker-section ABI;
- all instrumentation compiles out in release builds;
- debug failures produce the two-path causal trace with source location;
- a post-build static filter, if pursued later, can read constexpr metadata
  emitted into linker-set sections by the bridge and raise macros, merged
  across translation units at link time; it is an optimization, not a
  prerequisite;
- any static-check CMake integration should live in a self-contained directory
  imported by the parent `CMakeLists.txt`.
