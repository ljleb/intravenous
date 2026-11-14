# Scoped GraphBuilder And SubgraphClosure Direction

_Status: post-GraphJit/executor direction only. Do not begin this API/state
migration until GraphJit plus the realtime/background executors have landed as the stable execution
path and a substantial profiling/optimization iteration has been completed over
the then-current graph configuration/build pipeline. After that optimization
pass, this GraphBuilder API/state migration is the next planned builder-level
architectural step._

> **Implementation status (2026-11-06):** the measurement gate was satisfied by the
> builder/configuration baseline recorded under Front 12 in
> [implementation_fronts_direction.md](./implementation_fronts_direction.md), and the
> migration in this document is implemented and verified; Front 12 closed on
> 2026-11-06. The front's record lists the implementation decisions taken within this
> direction (path-qualified virtual IDs, wrapper-free registered invocations) and the
> deferred nested `node_reference` extension (F12-7). The transactional rollback this
> direction originally required was replaced on 2026-11-06 by stopping the
> configuration pass on failure; see "Construction failure stops the configuration
> pass" below.

This document defines the intended successor to the current child-session/live-
child embedding model. It supplements the implemented behavior described in
[graph_builder_embedding_and_matchers.md](./graph_builder_embedding_and_matchers.md),
[node_ref_dsl_contract.md](./node_ref_dsl_contract.md), and
[node_definitions_and_instances_direction.md](./node_definitions_and_instances_direction.md).
Until this migration starts, those documents' implementation checkpoints remain
descriptive of the current repository.

The central model is:

> `BuilderSession` is the only live mutable graph-construction universe.
> `GraphBuilder` and `SubgraphBuilder` are views over that universe and its
> nested scopes. `ConfiguredGraph` is a completely closed reusable graph value.

The purpose of the change is not to force every construction path through one
storage representation. The purpose is to give every source-level construction
path the same graph semantics while preserving direct/efficient implementations
for live scopes and frozen graphs.

## Sequencing gate

This work is deliberately deferred.

The required order is:

1. finish GraphJit plus the realtime/background executors as the normal whole-project execution path;
2. run a substantial optimization/profiling iteration over that landed runtime
   and the current configuration/build pipeline, including cache behavior and
   builder/module-build costs;
3. use the resulting measurements to preserve or improve important fast paths;
4. then make the GraphBuilder API/state changes in this document.

The API migration must not be pulled forward merely because it simplifies the
conceptual model. In particular, the future design must preserve the principle
that a cached `ConfiguredGraph` can be imported directly and that a live same-
session scope does not need to be copied merely to conform to a common internal
representation.

## Invariants

The redesign is governed by these invariants.

1. One `BuilderSession` owns one live graph-construction transaction.
2. Nested modules and nested subgraphs normally construct directly in that
   session rather than in independent child sessions.
3. `GraphBuilder` and `SubgraphBuilder` do not own graph state.
4. Every live `NodeRef`, `SamplePortRef`, and `EventPortRef` is owned by a
   `BuilderSession`, not by a particular builder-view object.
5. Different views of the same session are compatible; references from
   different sessions are rejected immediately.
6. `BuilderSession` never stores unresolved caller-local/foreign graph
   references. Cross-scope references inside one session are already-resolved
   ordinary graph references.
7. `ConfiguredGraph` is always closed and self-contained. It contains no live
   builder/session references, no free captures, and no deferred resolution.
8. Producing a `ConfiguredGraph` from a live `GraphBuilder` is a non-destructive
   snapshot/copy of the represented scope. It never removes that state from the
   `BuilderSession`.
9. Cache misses execute directly in the live session. Cache hits may substitute
   a direct `ConfiguredGraph` import. Those two paths must have equivalent graph
   semantics.
10. Construction failure stops the configuration pass. Nothing a failed
    construction did is undone in place; the pass's `BuilderSession` is discarded.

## Object model

The intended live/frozen split is approximately:

```text
BuilderSession
    one mutable graph construction
    owns:
        bundles
        connections
        sample/event expression tables
        public/interface boundary state
        annotations/source metadata
        virtual-node/introspection records
        configuration lifetimes
        package/definition configuration context
        nested scope stack
        first construction failure

        |-- GraphBuilder view
        |       BuilderSession*
        |       optional interface ScopeHandle
        |
        |-- GraphBuilder view
        |       same BuilderSession*
        |       another interface ScopeHandle
        |
        `-- SubgraphBuilder view
                same BuilderSession*
                one specific interface ScopeHandle

NodeRef / SamplePortRef / EventPortRef
    BuilderSession*
    graph-local handle/expression handle

ConfiguredGraph
    closed immutable graph value
    no BuilderSession*
    no live refs
    no free captures
    no deferred work
```

A root `GraphBuilder` has no nested interface scope. A module-construction
`GraphBuilder` names the module scope whose public interface its `input()`,
`event_input()`, `outputs()`, and `event_outputs()` operations edit.

`SubgraphBuilder` remains intentionally restricted to boundary construction. It
should expose only:

```text
input()
event_input()
outputs()
event_outputs()
```

It does not need to contain a `GraphBuilder&`. Its work is `BuilderSession* +
ScopeHandle -> manipulate that scope's boundary`.

## Dynamic construction scope versus interface scope

Two notions that are currently easy to conflate must remain distinct.

`GraphBuilder::input()`/`outputs()` operate on the interface scope represented by
that particular `GraphBuilder` view.

`GraphBuilder::node()`, graph connection creation, and other ordinary graph
mutation operate in the session's currently active construction scope.

Conceptually:

```text
view.interface_scope
    controls that view's public boundary API

session.current_scope()
    controls where newly constructed graph members belong
```

This is what permits a construction closure created in an outer module to be
invoked by an inner module without foreign references. If an outer `GraphBuilder`
view is captured and its `node()` API is used while an inner scope is current,
the new node belongs to the inner current scope because both views refer to the
same session.

Non-root views are still lifetime-bound to their represented scope. Interface
operations on a closed/stale scope must fail. The root view remains valid for
the construction transaction.

## BuilderSession owns a real nested-scope stack

The current `SubgraphBuildScope` is not sufficient as the long-term construction
context. `BuilderSession` should own explicit LIFO scope frames, conceptually:

```cpp
struct ScopeFrame {
    ScopeHandle id;
    ScopeHandle parent;
    NodeBundleHandle boundary;
    size_t child_begin;
    ScopeIdentity semantic_identity;
    // scope-local interface/finalization state...
};
```

Construction follows strict nesting:

```text
begin scope
    push frame

node()/connections
    mutate session under scope_stack.back()

begin nested scope
    push frame

finish nested scope
    finalize in place
    pop frame

continue parent
    scope_stack.back() is the parent again
```

The scope hierarchy is semantic infrastructure, not merely exception-handling
state. It supplies nested placement identity, virtual-node disambiguation, and
the boundary required to decide whether a scope can be snapshotted independently.

## Refs become session-owned

The current builder-view ownership model must change before multiple legitimate
`GraphBuilder` views can coexist over one live graph.

The target identity is conceptually:

```cpp
struct NodeRef {
    details::BuilderSession* session;
    NodeBundleHandle bundle;
};

struct SamplePortRef {
    details::BuilderSession* session;
    SampleExpressionHandle expression;
};

struct EventPortRef {
    details::BuilderSession* session;
    EventExpressionHandle expression;
};
```

The compatibility rule becomes:

```text
lhs.session == rhs.session
```

rather than equality of the particular `GraphBuilder*` façade that happened to
create a ref.

Consequently, `GraphBuilderState::_facade`, `bind()`, `facade()`, and ref methods
that route semantic operations back through the original façade should disappear
or become session/state operations. Temporary nested builder views must not be
part of live-reference identity.

Refs from genuinely different sessions remain incompatible. The redesign does
not introduce cross-session live wiring.

## One subgraph concept: two closure alternatives and in-place scope finalization

_Accepted design change (2026-11-06): a nested live `GraphBuilder` is no longer a
`SubgraphClosure` alternative; it is finalized by its own `g.subgraph` overload. The
superseded three-alternative text and its rationale are preserved in
[Front 12 SubgraphClosure With A Live GraphBuilder Alternative](./historical/front_12_subgraph_closure_live_builder_alternative_2026-11-06.md)._

Every source-level subgraph is one concept: `g.subgraph(...)` returns a `NodeRef` to
one subgraph bundle placed in the session's current scope. It accepts two kinds of
argument:

```cpp
NodeRef GraphBuilder::subgraph(SubgraphClosure&);    // reusable through an lvalue
NodeRef GraphBuilder::subgraph(SubgraphClosure&&);
NodeRef GraphBuilder::subgraph(GraphBuilder&& nested); // finalize a nested live scope
```

`SubgraphClosure` is a move-only, variant-backed value with implicit construction
from two semantic alternatives:

```text
ConfiguredGraph          // frozen; borrowed or shared ownership
ErasedSubgraphFunction   // move-only callable with SubgraphBuilder&
```

Both alternatives are reusable: every invocation places a fresh scope with fresh
bundles. A `SubgraphClosure` is therefore the value that can be stored and passed
through module scopes as a construction argument:

```cpp
SubgraphClosure voice = [&](SubgraphBuilder& boundary) { /* ... */ };
auto bank = g.node<"voice_bank">(voice);
```

A callable passed directly to `g.subgraph` is the same callable alternative without
type erasure.

Do **not** normalize these paths to a child `GraphBuilder` merely for implementation
uniformity. Each path has distinct efficiency/correctness needs.

### ConfiguredGraph alternative

A `ConfiguredGraph` is already closed reusable graph data. `g.subgraph(frozen)`
should import it directly through the configured-graph component importer and
handle translation machinery.

Do not rebuild a temporary `GraphBuilder` from the frozen graph.

Repeated placement is naturally valid:

```text
g.subgraph(configured)
g.subgraph(configured)
g.subgraph(configured)
```

Each placement creates fresh live bundles with an explicit local-to-session
translation.

### C++ construction-closure alternative

The erased callable path opens a fresh scope, constructs a restricted
`SubgraphBuilder` view for its boundary, invokes the callable, then finalizes the
scope. Source syntax such as the current form remains valid:

```cpp
g.subgraph([&](SubgraphBuilder& subgraph) {
    auto x = g.node<"processor">();
    subgraph.outputs(x);
});
```

The callback receives only boundary operations. Ordinary node creation occurs
through a captured/live `GraphBuilder` view, and `session.current_scope()` places
those nodes in the just-opened subgraph.

The erased callable should not be forced through `std::function` if that would
exclude useful move-only captures. A suitable move-only erased callable is
preferred where the toolchain permits it.

### Finalizing a nested live GraphBuilder in place

A nested `GraphBuilder` represents state that already exists in the same
`BuilderSession`. `g.subgraph(std::move(nested))` closes and finalizes that existing
scope **in place**. Its bundles are not copied and are not removed/reintroduced.

This is a one-shot scope-finalization operation, not a closure value. While the
nested scope is open it is the session's current scope, so every node constructed
anywhere in the session belongs to it. It cannot be passed into another node's
construction, whose own scope would open inside it; it can only be finalized at the
level where it is open. Once closed, the same live scope cannot be attached again.
Its graph contents are not consumed; they remain in the owning session.

The normal cache-miss module path finalizes its definition scope in place through the
same scope operation.

## Higher-order SubgraphClosure arguments

`SubgraphClosure` is explicitly intended to be passable as a construction
argument through nested module scopes.

One planned consumer is a paired STFT module. It accepts transform size, hop size,
inverse/window configuration, and a spectral-processing `SubgraphClosure`; internally
it constructs explicit forward and inverse FFT nodes with the same transform and hop
arguments and places the supplied closure between their `FFTBlock` ports. The closure
therefore removes duplicated user configuration without turning FFT conversion into an
implicit port operation or making hop size an inferred pace variable. This module is
implemented only after the general `SubgraphClosure` transport and lifetime contract in
this document exists.

For example, A may create an A-local source and a construction closure that uses
it, then pass that closure to B. B may invoke the closure several times:

```text
A scope
    source
    construct voice closure capturing source
    construct B(voice closure)

B scope
    voice invocation 0
    voice invocation 1
    voice invocation 2
```

Because B's cache-miss construction and each voice invocation occur in the same
`BuilderSession`, the captured A `NodeRef` and newly created voice nodes have the
same session owner. Connections are ordinary resolved same-session graph edges.

There are no:

```text
foreign port records
parent-builder pointers in ConfiguredGraph
deferred bindings
external objects
"resolve this when embedded into A" states
```

Each invocation opens a fresh nested scope and therefore creates fresh local
bundles and a fresh boundary. Ordinary positional repetition does not require an
explicit instance key.

A B scope containing a connection to an A-local node is valid live graph state,
but B is not independently snapshotable as a `ConfiguredGraph`; it has a free
graph capture. A larger enclosing scope that contains both ports may still be
self-contained and snapshotable.

## ConfiguredGraph remains absolutely closed

`ConfiguredGraph` is a completed reusable configuration value. It may contain
its own bundles, its own connections, nested scope structure, public boundaries,
virtual-node/introspection records, annotations, and configuration data.

It must never contain:

```text
BuilderSession*
GraphBuilder*
NodeRef / SamplePortRef / EventPortRef
an ancestor-scope bundle handle
an unresolved parent binding
a C++ construction closure
deferred builder work
"resolve when embedded into builder/session X"
```

This keeps GraphJit, the realtime/background executors, connection planning, storage planning, binary
archive support, and other post-configuration systems unaware of construction-
time closure/capture mechanics.

## Non-destructive ConfiguredGraph snapshot API

A configured live `GraphBuilder` must expose a way to produce a `ConfiguredGraph`
for the scope it represents **without consuming, extracting, or moving that scope
out of the BuilderSession**.

The exact API name may be chosen later; conceptually:

```cpp
expected<ConfiguredGraph, ConfiguredGraphSnapshotError>
GraphBuilder::configured_graph() const;
```

This operation is a snapshot/copy.

It:

1. determines the complete scope subtree represented by the builder view;
2. checks structural closedness/self-containment;
3. copies the represented bundles and all semantic graph components into a new
   graph-local namespace;
4. remaps all copied handles/references into that namespace;
5. returns an independent `ConfiguredGraph`;
6. leaves every live bundle, connection, expression, annotation, and scope in
   `BuilderSession` untouched.

This operation is only viable when the represented scope is self-contained.
At minimum, the closedness analysis must reject a snapshot when any semantic
reference escapes the scope subtree, including:

- sample/event connection ports in an ancestor/outside scope;
- public output expressions that reference outside bundles;
- detach/feedback expressions that reference outside the subtree;
- virtual/introspection membership that cannot be represented locally;
- configuration objects containing illegal live builder/session references;
- any equivalent future graph-semantic reference.

Nested descendant scopes are local and do not by themselves make a snapshot
open.

For an explicit snapshot request, closedness failure should produce a diagnostic
that identifies the free graph capture. For cache population, the same condition
normally means "this invocation is not independently cacheable" rather than
"construction failed".

### Why snapshot instead of extraction

The cache-miss path must not do this:

```text
construct scope in BuilderSession
    -> remove/extract scope state
    -> form ConfiguredGraph
    -> embed ConfiguredGraph back into the same BuilderSession
```

That would move/copy state out merely to recreate the graph that already exists.

The desired asymmetry is:

```text
live scope -> ConfiguredGraph
    non-destructive copy/snapshot because both values must continue to exist

ConfiguredGraph -> live scope
    copy/remap because the frozen graph is reusable

live nested scope -> enclosing live graph
    zero-copy finalization because it already resides in that BuilderSession
```

The current invocation therefore continues using the nodes it constructed. The
snapshot exists only for later reuse.

## Cache miss and cache hit become equivalent implementation choices

Registered `g.node(...)` calls currently rely heavily on detached configuration
and frozen embedding. After this migration, cache lookup and live construction
must be separated.

A cache hit remains:

```text
g.node<module>(...)
    -> cached ConfiguredGraph
    -> direct configured import/remap into current BuilderSession
    -> NodeRef
```

A cache miss becomes:

```text
g.node<module>(...)
    -> begin nested scope in current BuilderSession
    -> nested GraphBuilder(session, scope)
    -> invoke provider/module construction
    -> finalize live scope in place
    -> NodeRef for already-created live state

    if reusable and structurally self-contained:
        snapshot/copy represented scope -> ConfiguredGraph
        store snapshot for later calls
```

The exact ordering of successful scope finalization versus cache snapshot may be
chosen to give clean exception safety, but it must never require removing the
scope and reimporting its snapshot for the current invocation.

This makes caching an optimization in the strong sense:

```text
miss: execute construction directly
hit:  substitute an equivalent frozen result
```

Moving a future cache nearer LLVM IR must therefore not require changing source-
level construction semantics.

## Constructibility, retention, and cache-keyability are different properties

The current caching machinery already distinguishes some ownership/equality
cases, but higher-order closures require the conceptual split to become explicit.

A construction argument may be:

```text
constructible
    valid to pass synchronously to provider/module construction

retainable
    safe to retain for the graph/configuration lifetime that actually needs it

cache-keyable
    has stable semantic equality/hash suitable for reusable cache lookup
```

These properties must not be collapsed into one requirement.

A `SubgraphClosure` capturing live `NodeRef`s is a normal example of a useful
constructible argument that is usually not cache-keyable at the current
`ConfiguredGraph` cache layer. That makes the affected invocation uncached; it
does not make it invalid to construct.

Provider/configuration objects that legitimately retain ordinary argument data
still require lifetime ownership. Same-session construction therefore needs a
session/configuration-lifetime owner equivalent to the lifetime guarantees
currently pinned by configured cache entries. A live graph-construction closure,
however, is construction-time behavior and must not survive as an object inside a
finished `ConfiguredGraph` or runtime leaf state.

The existing cache-entry wrapper may continue to pin provider revisions,
configuration argument storage, and dependency lifetimes around a
`ConfiguredGraph`; this direction does not require those metadata/lifetime
concerns to become fields of `ConfiguredGraph` itself.

## Construction failure stops the configuration pass

_Accepted design change (2026-11-06): this replaces the transactional rollback this
direction originally required. The superseded section and its rationale are preserved
in [Front 12 Transactional Construction Rollback](./historical/front_12_transactional_rollback_2026-11-06.md)._

Independent child sessions used to provide an implicit failure boundary: a failed
child could be destroyed without contaminating its parent graph. Same-session
construction does not replace that boundary with rollback. A failed construction is
not recovered inside its `BuilderSession`. Its failure stops the whole configuration
pass, and the pass's session is discarded.

A construction failure is an exception that leaves any of these:

- a session operation, such as creating, connecting, or annotating a node, looking
  up a port, declaring an interface, or finalizing a scope or the root;
- a module or leaf construction factory;
- a `SubgraphClosure` invocation;
- a registered call (`g.node<"id">(...)`).

The exception may come from the library, for example an unknown port name, a missing
nested definition, a definition cycle, or tiled members with incompatible interfaces.
It may also come from authored provider code. Newly settled detail (2026-11-06): an
error raised by a session operation fails the session whoever called it, host code
included. Without rollback, the operation may already have changed the session.

The first failure is recorded on the session before the exception leaves the
library boundary it crosses. A session with a recorded failure refuses every further
construction operation. Module code therefore cannot resume construction by catching
the exception: its next builder call fails again. A factory that returns normally
after a failure was recorded still fails its pass. An exception that authored code
raises and handles itself, without involving the builder, is not a construction
failure.

At the pass boundary:

- `NodeInstances` first checks every request for problems that need no
  construction: configuration source that does not parse or type-check, an unknown
  definition, or a project-node reference to an instance the batch does not declare.
  It reports all of them together. A source that contains project-node references
  is type-checked only once the instances it references are placed, so its type
  errors are construction failures. Any such problem fails the pass before
  construction starts. Otherwise, the first construction failure stops construction
  and is reported.
- A recorded project connection that does not currently resolve to compatible
  ports remains retained intent with a diagnostic, as before. Either a matcher
  resolves to no port, or the resolved ports' types differ from the connection's.
  Resolution and type compatibility are checked before any mutation. A failure
  while applying a resolved, compatible connection fails the pass. Newly settled
  detail (2026-11-06): a type mismatch is treated like a missing endpoint, so a
  package update that retypes a port behaves like one that removes it.
- `ProjectGraph` discards the root `BuilderSession`, retains the desired revision
  and the diagnostics, publishes no new configured generation, and sends nothing to
  either executor. The previous configured and executable generations remain
  current, as they do after a `GraphJit` compile failure. On a project's first build
  there is no previous generation, so nothing runs until the failure is fixed.

A failure diagnostic contains the failure message, the construction context (the
project instance ID, the chain of definition IDs being configured, and the semantic
path), and a stack trace that shows where the failure was raised. A library-detected
error can capture that stack where it is detected. An exception raised by authored
code is already unwound by the time a boundary catches it, so its stack must be
captured when it is thrown, for example by intercepting `__cxa_throw` on Itanium-ABI
platforms.

Consequences:

- No ref can denote discarded state, because nothing is truncated and reused.
- A `GraphBuilder::nested()` view destroyed without being finalized records a
  failure.
- Tiled module construction is all-or-nothing without its own checkpoint: a member,
  configuration, or interface failure fails the pass, so no partial tile is
  published.
- A failed invocation populates no cache entry. Entries populated by nested
  invocations that succeeded before the failure remain valid reusable values.
- Snapshot closedness (see the snapshot API section) is purely structural. No
  transaction exists whose effects outside the scope a snapshot could miss.

## Virtual-node identity must become scope-aware

Cache hits and cache misses must not produce different virtual-node grouping.

Today live virtual-node creation can coalesce by source/type identity across a
builder state, while imported frozen graphs are deliberately cloned/remapped with
scope-qualified introspection identity. If cache misses begin constructing
multiple independent modules directly in one session, source-identical virtual
nodes from separate module scopes must not accidentally collapse together while
cache-hit imports remain distinct.

The future identity must therefore include semantic construction scope:

```text
virtual identity =
    enclosing semantic scope path
    + source declaration identity
    + concrete type identity
```

Within one semantic scope, repeated concrete members generated from the same
annotated declaration may still belong to one virtual node when that is the
existing source-level meaning. Across independent module/subgraph scopes they do
not coalesce.

This same scope-aware identity is the appropriate seam for source navigation,
presentation focus, state reconciliation, and cache-hit/cache-miss equivalence.

## Automatic repetition identity

Ordinary repeated subgraph construction should continue to require no explicit
key:

```cpp
for (size_t i = 0; i != voices; ++i)
    g.subgraph(factory);
```

Each invocation opens a concrete child scope. Its identity can be derived from
the stable enclosing semantic path, conceptually:

```text
authored module/project instance
/
annotated construction site
/
invocation/member index
/
nested annotated path
```

Index identity is correct for genuinely positional structures such as
`voice[0]`, `voice[1]`, and `voice[2]`.

There is an unavoidable limitation: if a future abstraction represents
reorderable semantic entities, removing/reordering entries cannot preserve entity
identity from indices alone. That particular abstraction may expose explicit
stable member identity. Such keys must not become mandatory for ordinary
subgraph construction.

## Runtime factory signature constraints are validation, not construction semantics

A module may require that a supplied `SubgraphClosure` produce a boundary with a
particular structural interface. That does not require a second construction
system.

The initial model should remain:

```text
SubgraphClosure
    -> invoke with g.subgraph(...)
    -> NodeRef/subgraph boundary
    -> inspect derived public interface
    -> validate against required signature
```

"Match or exceed" should use the normal structural port compatibility/join rules
rather than exact interface equality when the caller's requirement permits it.

A future typed convenience such as `SubgraphClosure<VoiceSignature>` is optional.
If added, it must wrap the same invocation primitive rather than introduce a
parallel graph representation.

Lexically captured graph references are not part of the produced subgraph's
public functional interface. They are live construction environment and either
remain resolved in the containing live session or prevent independent
`ConfiguredGraph` snapshotting.

## Root construction and finalization

The root graph uses the same live session model without a parent interface scope.
Root finalization remains allowed to produce the final closed `ConfiguredGraph`
for lowering/execution.

The non-destructive scope snapshot API is separate from root/session ownership
transfer. An implementation may retain a consuming whole-session finalization
path where it is useful at the true root; that must not be reused as the cache
snapshot mechanism for a nested live scope.

## ConfiguredGraph embedding remains direct

The current direct frozen-graph importer remains a required fast path after this
migration.

A frozen placement continues to:

- append/copy the configured graph's local bundles into the live session;
- translate configured-local handles to fresh live handles;
- import sample/event connections, detach state, public boundary data,
  annotations, virtual-node state, and nested scope identity;
- return explicit translation/embedding information where project matchers need
  it.

What disappears is the need to treat a newly constructed live child module as an
independent graph that must go through that importer. Live same-session scopes are
already in place and are finalized directly.

## Consequences for NodeInstances/configuration caching

The current `NodeInstances` implementation remains valid until the sequencing
gate at the top of this document is reached. During the later migration:

- recursive cache misses stop creating independent root `BuilderSession`s solely
  to obtain a `ConfiguredGraph` before placement;
- nested registered module calls normally open scopes in the current session;
- a reusable cache entry still stores a closed `ConfiguredGraph` plus the
  required argument/provider/dependency lifetimes and provenance;
- non-keyable arguments configure normally without reusable value sharing;
- live captures may make one nested scope unsnapshotable while a larger enclosing
  scope remains snapshotable;
- cache-hit import and cache-miss direct construction must be tested for semantic
  equivalence, especially virtual-node and matcher-visible hierarchy identity.

No cache layer is allowed to make `ConfiguredGraph` heterogeneous or partially
resolved in order to preserve hit rate.

## Approaches explicitly rejected

The following should not be reintroduced as shortcuts during implementation.

### Foreign references inside ConfiguredGraph

Do not store owner-qualified references to parent/ancestor builder bundles in a
`ConfiguredGraph` and resolve them during a later embedding. That makes the
configured value context-dependent and leaks construction-time semantics into
later passes.

### Converting ConfiguredGraph to GraphBuilder before import

Do not rebuild a temporary live builder just to reuse live-scope attachment code.
The frozen graph already contains the information required by the direct importer.
Share lower-level remapping routines instead.

### Extracting a live scope merely to cache it

Do not remove live state from `BuilderSession`, turn it into a `ConfiguredGraph`,
and immediately import it back for the current invocation. Snapshot the live scope
non-destructively and keep using the original live state.

### Treating cacheability as a precondition for construction

A useful argument or closure may be synchronous/constructible without having a
stable hash/equality representation. Reconfigure rather than rejecting valid
construction.

### Global source-only virtual identity

Do not let direct same-session construction coalesce independent nested module
instances merely because their annotated source/type identities match. Scope is
part of semantic identity.

## Migration outline after the sequencing gate

Once GraphJit/executor are complete and the required large optimization pass
has established the performance baseline, the intended builder migration order is:

1. introduce session-level live-ref identity and remove dependence on
   `GraphBuilder*` façade identity;
2. make `GraphBuilder`/`SubgraphBuilder` explicit non-owning session/scope views;
3. add the session-owned LIFO construction-scope stack (the transactional
   checkpoints/rollback originally planned here were replaced on 2026-11-06 by
   stopping the configuration pass on failure);
4. make virtual-node identity scope-aware before cache misses can construct
   multiple independent modules directly in one session;
5. add `SubgraphClosure` and the single semantic `g.subgraph(...)` surface while
   retaining separate direct implementations for the frozen and callable closure
   alternatives and for in-place finalization of a nested live scope;
6. add the non-destructive, closedness-checked `GraphBuilder -> ConfiguredGraph`
   snapshot API;
7. split configuration argument constructibility/retention/cache-keyability in
   the configuration/cache layer;
8. change registered-module cache misses to nested same-session construction;
9. populate reusable cache entries by snapshotting successful self-contained
   live scopes, without reimporting them for the current invocation;
10. retain configured-graph direct import as the cache-hit/repeated-placement
    path and prove hit/miss equivalence with focused tests;
11. remove obsolete child-session/live-child embedding machinery only after all
    call sites and failure/tiling tests use the new scoped path.

The optimization pass before this migration is part of the plan, not an optional
cleanup afterward. It should identify current hot paths and establish benchmark
coverage so this structural cleanup does not regress builder/configuration cost in
pursuit of API uniformity.

## Completion criteria

This direction is complete when all of the following are true:

- a normal nested registered-module cache miss creates no independent child
  `BuilderSession`;
- cache hits still import `ConfiguredGraph` directly;
- a live self-contained nested `GraphBuilder` can be snapshotted into a
  `ConfiguredGraph` without altering its `BuilderSession` state;
- a live scope with a free ancestor capture remains constructible but cannot be
  independently snapshotted;
- `NodeRef`/sample/event refs remain valid across different builder views of the
  same session and reject different sessions;
- a construction failure stops the configuration pass, module code cannot resume
  a session after one, and no partial graph from a failed pass is published;
- direct cache misses and frozen cache hits produce equivalent nested/virtual
  identity visible to project matchers and introspection;
- higher-order `SubgraphClosure` arguments can be invoked repeatedly in deeper
  scopes without foreign/deferred graph references;
- `ConfiguredGraph` remains completely closed and requires no awareness of this
  construction mechanism from GraphJit or the executor modules.
