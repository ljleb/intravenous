# Project Graph Application Architecture

_Status: current project-graph architecture and implementation direction._

This document is the authoritative application-module design for the unified
project graph after deletion of the legacy execution stack. Where it conflicts
with older execution-stack or iv-module-specific notes, this document
wins.

Related documents:

- [DSP Execution And Storage Glossary](./dsp_execution_storage_glossary.md)
- [event_propagation_tree_constraint.md](./event_propagation_tree_constraint.md)
- [package_pipeline_architecture.md](./package_pipeline_architecture.md)
- [node_definitions_and_instances_direction.md](./node_definitions_and_instances_direction.md)
- [graph_builder_embedding_and_matchers.md](./graph_builder_embedding_and_matchers.md)
- [system_audio_devices_direction.md](./system_audio_devices_direction.md)
- [graph_jit_direction.md](./graph_jit_direction.md)
- [realtime_background_execution_and_queues.md](./realtime_background_execution_and_queues.md)
- [sequential_port_storage_planning.md](./sequential_port_storage_planning.md)
- [startup_realization_order.md](./startup_realization_order.md)
- [unified_graph_direction.md](./historical/unified_graph_direction.md)
- [event_flows/README.md](./event_flows/README.md)

## Terminology

The application should use **node** as the general term for both primitive IV
node definitions and graph-producing IV module definitions.

| General term | Primitive specialization | Graph-producing specialization |
| --- | --- | --- |
| node definition | leaf node definition | module node definition |
| node instance | leaf node instance | module node instance |

`IV_NODE` and `IV_MODULE` remain the source/API registration spellings. The
terms above describe the application model.

A module node is a project/configuration node, not an execution partition. A
whole-project optimizer may inline, fuse, reorder, or otherwise lower across
module-node boundaries. Stable project identity and source provenance must
survive that lowering independently of runtime call boundaries.

The `Iv` prefix is no longer useful on generalized application modules. Planned
names therefore use:

- `PackageWatcher`
- `PackageJit`
- `PackageDefinitions`
- `NodeDefinitions`
- `NodeInstances`

`IvModuleSourceIntrospection` keeps its existing name because only module nodes
have this source-level introspection contract; leaf nodes do not. IV-specific
names also remain appropriate where they identify an actual source format,
registration API, package ABI, or compiler concept.

## App-module inventory

The core package/project-graph modules are:

| Module | Primary responsibility |
| --- | --- |
| `PackageWatcher` | own package source/watch state and coordinate one complete package-refresh transaction for every build cause |
| `PackageJit` | synchronously build/JIT one requested package batch into immutable package revision results |
| `PackageDefinitions` | own accepted package revisions, package catalog/build state, and one immutable package-revision snapshot |
| `NodeDefinitions` | derive the immutable global node-definition namespace from accepted package revisions |
| `ProjectGraph` | orchestrate one complete root-graph construction transaction without duplicating instance/connection intent |
| `NodeInstances` | own desired node-instance state, instantiate one complete batch against exactly one definitions snapshot, and own reusable configured node-instance caches |
| `GraphConnections` | own desired cross-node connection state, resolve project-wide port matchers against one complete root embedding, and apply those connections |
| `GraphJit` | synchronously lower, optimize, and ORC-JIT one complete root `ConfiguredGraph` into an immutable `CompiledGraph` generation |
| `RealtimeGraphExecutor` | own active/pending realtime generations, realtime mutable node storage, pass execution, state migration, safe-boundary activation, and realtime producer endpoints |
| `BackgroundGraphExecutor` | own asynchronous background execution, independently queued producer inputs, background mutable state/work selection, persisted-data computation, and publication of immutable persisted-state versions |
| `IvModuleSourceIntrospection` | derived source/logical-node read model for module nodes only |
| `SystemAudioDevices` | own system-audio enumeration, stable logical device bindings, hardware-device lifetime, buffering, and synchronization |
| `ProjectPersistence` | load/save normalized persistent state without becoming the canonical owner of instance/connection intent |
| `ProjectAutosave` | coalesce configured-state mutations and request persistence |
| `SocketRpcServer` | transport JSON-RPC requests/notifications without becoming the owner of project state |

Future presentation modules may include `PresentationDefinitions` and
`PresentationInstances`. They are intentionally not required for the initial
project-graph implementation.

### Implementation checkpoints

The package-side app-module migration specified in
[package_pipeline_architecture.md](./package_pipeline_architecture.md) has landed:

```text
PackageWatcher
    +-> PackageJit
    `-> PackageDefinitions
            `-> NodeDefinitions
```

There is deliberately no fourth package coordinator module. `PackageWatcher` is
the natural coordinator because every package build can return a new dependency
set that changes the filesystem state it owns. Build results return synchronously
from `PackageJit`; `PackageWatcher` updates its watches and then enters
`PackageDefinitions` exactly once with the complete transaction.

`PackageDefinitions` now owns accepted package revisions and package-catalog
state; `NodeDefinitions` derives only the global namespace. The old
`IvPackageDefinitionsChanged` event survives temporarily only as a compatibility
projection to module-source introspection, not as package-state ownership.
`ProjectGraph` now consumes the immutable `NodeDefinitionsSnapshot` and passes
the pinned snapshot synchronously to `NodeInstances` during each root transaction.

Package dependency watching and package-root discovery both use Linux `inotify`.
`PackageWatcherService` blocks on filesystem descriptors plus explicit work and
shutdown eventfds; there is no periodic package-root discovery scan and no
portability polling fallback. Linux inotify is therefore a runtime requirement.

Direct frozen `ConfiguredGraph` embedding has also landed. Live child builders
and frozen graphs now share one importer/remapper; each placement returns an
explicit local-to-parent translation for node bundles/scopes and virtual nodes,
and imported virtual identities remain distinct across child scopes. This
removes the live-`BuilderSession` obstacle to cacheable `NodeInstance` values.

The `NodeInstances` configuration core has now landed as well. The application
module is generalized/renamed, consumes complete immutable definition snapshots,
owns provider-generated typed argument values, recursively resolves nested
configuration through one snapshot, caches immutable configured graphs by typed
value, and uses the frozen-graph embedding translation for repeated placements.

The first `ProjectGraph` coordinator checkpoint has now landed too. Runtime
definition publication enters `ProjectGraph`, not `NodeInstances`; node create,
delete, and update commands from persistence/RPC also enter `ProjectGraph`. For
each cause it creates a fresh root builder, invokes `NodeInstances` exactly once
with the latched snapshot and optional mutation, finishes one immutable root
`ConfiguredGraph`, and retains that root generation plus placements/diagnostics.
`NodeInstances` remains the canonical desired-instance owner and keeps only its
read/persistence projection surfaces outside that synchronous child operation.

The first `GraphConnections` checkpoint has now landed. It owns the canonical
desired cross-node connection set, resolves structured set-valued
`ProjectNodePortMatcher`s only after the complete instance set has been embedded,
applies all currently resolvable sample/event connections to the same fresh root
builder, and retains dangling connection intent with diagnostics. `ProjectGraph`
invokes it exactly once after `NodeInstances` and retains the resulting applied-id
and diagnostic batch on the immutable root generation.

The `GraphJit` application/compiler shell has now landed as the next sibling
stage. `ProjectGraph` synchronously offers each completed root generation through
a singleton request/response event and retains either the resulting immutable
`CompiledGraph` or structured compile diagnostics. `GraphJit` already owns exact
package-LLVM provenance resolution, retained-global relocation resolution, O3,
and per-generation ORC lifetime. The isolated `ConfiguredGraph` + resolved node
LLVM -> project LLVM lowering function is intentionally still pending. Before
that lowering body lands, the provisional shell storage/entrypoint contract is
to be collapsed onto the existing node runtime model: the generated project is a
zero-input/zero-output root node; lowering finalizes the canonical `NodeLayout`
*before* final LLVM generation by executing the exact accepted declaration
callbacks and declaring compiler-owned raw regions. `RealtimeGraphExecutor` owns the
realtime `NodeStorage`; `BackgroundGraphExecutor` owns its private background
realization/workspaces and reaches internal outputs through specialized background
evaluation component metadata rather than a synthetic project-root `tock_coverage()`.
Final layout offsets are therefore compile-time constants in the generated LLVM. The
current implementation may temporarily retain a monolithic executor while these
responsibilities are split, but the two-executor ownership model is normative. Structured connection persistence/JSON-RPC adapters are also still
pending; the typed project command surface and canonical connection owner now
exist so those adapters do not need to invent connection semantics. The existing
line-oriented `ProjectPersistence` loader still replays legacy node commands one
at a time; collapsing those commands into the one normalized startup replay batch
described below remains a persistence-side checkpoint. C++ argument-list
expression compilation remains an independent internal `NodeInstances`
checkpoint; `IvModuleSourceIntrospection` remains a later read-model migration
checkpoint.

## `ProjectGraph` is the root-graph transaction coordinator

There is no separate `RootGraph` app module.

`ProjectGraph` does **not** duplicate the desired instance and connection sets.
Those are owned by the modules that interpret them:

- `NodeInstances` owns desired node-instance batches;
- `GraphConnections` owns desired cross-node connection batches.

`ProjectGraph` owns the transaction that combines those independently maintained
states into one coherent root graph. It also latches the latest immutable
`NodeDefinitionsSnapshot` delivered by `NodeDefinitions` so one complete root
transaction uses exactly one definition world.

Its root-build procedure is always batched:

1. create one fresh root `GraphBuilder`;
2. invoke `NodeInstances` exactly once, optionally carrying an instance mutation
   or replay batch, and have it populate/embed its complete current desired set
   using exactly the latched `NodeDefinitionsSnapshot`;
3. invoke `GraphConnections` exactly once after all instances have been
   considered, optionally carrying a connection mutation/replay batch, passing
   the same root builder and complete embedding map;
4. finish the root builder into one `ConfiguredGraph`;
5. invoke `GraphJit` exactly once to synchronously compile that graph into one
   immutable `CompiledGraph`;
6. stage that immutable generation once in `BackgroundGraphExecutor`, which prepares
   the background successor, generation-specific queues/bindings, target-generation identity/route metadata,
   and allocation-free cutover-publication resources;
7. only after background staging succeeds, stage the same generation once in
   `RealtimeGraphExecutor`, passing/identifying the prepared cutover so the realtime
   successor can later become activatable.

The downstream modules are siblings in the propagation tree. Their numeric
order above is execution order inside one `ProjectGraph` handler, not a
parent/child relationship between those modules.

An unavailable definition or temporarily unresolved matcher does not delete the
corresponding desired state from `NodeInstances` or `GraphConnections`. The
root transaction reports unresolved/failed configured state while the owning
module retains the user's request for a later generation.

## Tree-shaped event propagation is a hard constraint

For any single event source invocation, an app module may be entered at most
once. Propagation between app modules must form a strict tree, never a diamond,
DAG, or re-entrant graph.

Data may move in both directions along one control-flow edge. A request event
may pass mutable builders/result objects to a child and receive data back from
that child without creating a reverse event edge.

When a natural design produces a diamond such as:

```text
A -> B -> D
A -> C -> D
```

restructure the modules so the convergence becomes orchestration:

```text
A -> D -> B
     D -> C
```

`D` is then entered once and invokes `B` and `C` once each.

This is why `ProjectGraph` is the parent/orchestrator of `NodeInstances`,
`GraphConnections`, `GraphJit`, `RealtimeGraphExecutor`, and
`BackgroundGraphExecutor` for every execution-affecting graph-change cause. The
executor modules also communicate directly for later realtime-production and
background-completion causes; those are separate propagation trees, not convergence
inside the `ProjectGraph` cause.

Batches are the normal API shape. One logical graph change must not emit one
application event per node or per connection.

The fundamental event procedures are documented separately:

- [User node mutation](./event_flows/user_node_mutation.md)
- [Package refresh](./event_flows/package_refresh.md)
- [User connection mutation](./event_flows/user_connection_mutation.md)
- [Startup and project replay](./event_flows/startup_and_project_replay.md)
- [Realtime/background executor exchange](./event_flows/realtime_background_exchange.md)

## Definition snapshots

`NodeDefinitions` owns the current versioned immutable registry of every
registered leaf and module node definition.

A snapshot contains enough information for `NodeInstances` to perform all
recursive node construction without entering `NodeDefinitions` again. In
particular each definition entry needs, directly or through pinned provider
objects:

- stable definition id;
- definition kind (`leaf` or `module`);
- provider revision/version;
- construction/configuration callback;
- configuration signature/type identity;
- generated typed configuration operations;
- module/code/data lifetime references required to call the provider safely.

The snapshot is exchanged as one immutable object, conceptually:

```cpp
struct NodeDefinitionsSnapshot {
    uint64_t generation;
    DefinitionMap by_id;
};
```

A single `NodeInstances` batch uses exactly one snapshot. Nested calls to
`g.node<"...">(...)` during module evaluation use the same snapshot through an
ordinary in-process lookup/callback path, not another app-module event.

It must therefore be impossible for one batch to evaluate half of its nodes
against definition generation N and the other half against N+1.

## Node configuration arguments

Project-authored configuration arguments are persisted initially as one restricted
C++-like argument-list source string supplied by the UI, for example:

```cpp
OscillatorConfig{.frequency = 440.0f}, 0.25f
```

Do **not** split this string on commas in application code. The restricted grammar must
support nested braced initialization lists, registered aggregate/container values, and
other admitted expressions containing commas. A dedicated parser/evaluator handles the
whole argument list and type-checks it against the registered configuration signature.

This language is intentionally not general C++. It may provide scalar/string/enum
literals, numeric arithmetic, a small allow-listed set of pure `cmath`-like functions,
and framework-provided reference/query forms such as
`ref(user_authored_node_id)` or `select(virtual_node_name)[idx]...`. It does not admit
arbitrary statements, loops, mutation, lambdas, templates, allocation, arbitrary
function calls, filesystem/network access, or other general program execution. Exact
syntax is owned by the configuration-expression implementation front.

Provider-generated typed helpers can materialize/own values of the demanded C++ types
and invoke the existing erased configuration ABI. That preserves type-safe provider
ownership without treating persisted project text as arbitrary source code to compile.

Each registered configuration signature should expose a generated typed
operation table for owned erased argument tuples, approximately:

```text
copy
move (if useful)
destroy
equal
hash
```

Those operations are generated where the real C++ types are known. Do not use
`memcmp` as semantic argument equality.

The restricted source string can key the configuration-expression parse/evaluation cache. The
semantic node-instance cache should use provider version plus typed argument
values where equality/hash support is available. If an argument type cannot
provide safe value comparison, sharing may be disabled for that invocation;
re-evaluating the definition is correct.

Configuration expressions are declarative configuration, not a side-effect
execution API. Equivalent invocations may share one cached configured result,
so correctness must not depend on the number of times a configuration
expression happens to execute.

## `NodeInstances`

`NodeInstances` owns one complete batched instantiation phase and the reusable
configured node-instance cache.

A requested instance contains at least:

```text
stable external instance id
definition id
restricted configuration argument-list source
```

`NodeInstances` owns the canonical desired project instance set. `ProjectGraph`
may carry one typed mutation or replay batch into the current root-build
transaction, but it does not retain a duplicate desired-instance list.
`NodeInstances` also retains cache/configuration machinery and derived
diagnostics needed to make later batches efficient.

`NodeInstances` does **not** define one runtime DSP object per external instance
id. Multiple external instance ids may resolve to the same cached immutable
node instance:

```text
instance id A --\
                > cached NodeInstance X
instance id B --/
```

Embedding X twice into the root graph still yields two distinct placements and
therefore distinct runtime state/storage after lowering.

The cacheable `NodeInstance` should be based on a frozen `ConfiguredGraph`, not
a live `GraphBuilder`/`BuilderSession`. `BuilderSession` owns transient
construction state that should not be shared between embeddings, including
package/definition context, recursion stacks, pending allocations, expression
state, and builder-local handles.

A cached node instance therefore conceptually contains:

```text
owned typed configuration arguments
frozen ConfiguredGraph
stable local root handle/interface
provider/module lifetime references
definition generation/version provenance
```

On one batched request `NodeInstances`:

1. receives exactly one definitions snapshot;
2. recursively configures every cache miss using only that snapshot;
3. reuses value-equal cache entries where possible;
4. embeds every requested external instance into the supplied root builder;
5. returns one complete external-id -> embedding translation map plus batched
   diagnostics.

It does not emit one downstream event per instance.

## Embedding cached configured graphs

`ConfiguredGraph` should be directly embeddable into a parent `GraphBuilder`.
There should not be a `ConfiguredGraph -> temporary GraphBuilder -> parent`
round trip.

Live child-builder embedding and frozen configured-graph embedding should share
one low-level importer/remapper so there is one semantic path for introducing a
subgraph into a parent graph.

Local handles inside a cached node instance remain stable local identities.
Each embedding returns an explicit mapping from those local handles/scopes to
the corresponding parent-builder handles. Embedding the same cached instance
twice must therefore produce two independent mappings without mutating the
cached local handles.

See [graph_builder_embedding_and_matchers.md](./graph_builder_embedding_and_matchers.md).

## `GraphConnections`

`GraphConnections` owns the canonical desired cross-node connection set plus
project-wide connection **resolution and application** for one complete batch.
`ProjectGraph` may carry one typed connection mutation/replay batch into a root
transaction, but it does not retain a duplicate connection list.
`GraphConnections` may additionally retain derived resolution/cache/diagnostic
state that is useful across transactions.

Connections are applied **only after the complete requested node-instance batch
has been embedded**. This avoids transient failures caused by resolving a
connection before its target instance exists in the current root transaction.

Persistent connection references use `ProjectNodePortMatcher`, not raw
builder-local `NodeRef`s without introducing a parallel generic port-address vocabulary.

A matcher is set-valued. It selects a set of matching node ports through:

- one top-level external node-instance id;
- an arbitrary recursive node-path predicate;
- one port name/port matcher;
- an optional concrete port-channel selector.

The recursive path must preserve and navigate distinctions between:

- virtual-node identity;
- ordered direct members under a virtual node;
- tiled node children selected by channel/member identity; and
- nested subgraph scopes.

Selecting a tiled child and selecting a channel of a port are different
operations and must remain different in the representation.

For a sample connection:

- matched output channels from its output-side matcher list are source
  contributors and are summed according to normal graph semantics;
- the resulting source expression is broadcast/applied to all matched input
  channels from the input-side matcher list;
- source and target channel types are stored explicitly on the connection and
  validated before lowering/conversion.

Unresolved matchers remain desired connection state. If a node disappears on
reload, its connection must become dangling rather than being silently deleted
or retargeted. It may resolve again if the same stable identity/path returns.

## Recursive graph identity requirements

Arbitrary recursive child navigation is required, not optional.

Current virtual-node import behavior that flattens child virtual nodes by
identity is insufficient. Builder/configured-graph state must preserve a
hierarchical scope tree so paths such as the conceptual form:

```text
<instance-id>.virtual-a.virtual-b.<member-4>.<tile-left>.virtual-c
```

remain meaningful after embedding.

Ordered direct-member position under a virtual node is stable external identity
once it is persisted in project matchers. Reload/reconfiguration must therefore
preserve semantic member ordering for surviving members.

Tiled nodes must preserve child-node structure in `GraphBuilder` and
`ConfiguredGraph`. A tile child selector identifies a child node. It is not the
same as selecting channel N of a matched sample port.

## `GraphJit`

`GraphJit` is the whole-project compilation app module. `ProjectGraph` gives it
only a complete root `ConfiguredGraph` plus the exact provider/code provenance
used to construct that graph. `GraphJit` synchronously returns one immutable
`CompiledGraph`.

`GraphJit` owns the project-compilation ORC domain: a long-lived project
`LLJIT`, generation-specific `JITDylib`/resource-tracker state, graph-specific
LLVM generation/optimization, and the code-lifetime handles returned with each
compiled generation. This ORC state is separate from the package/configuration
JIT currently used to execute definition/configuration callbacks. The two JITs
run at different compiler stages and have different lifetime keys even though
they may share low-level LLVM helper code.

Compilation is intentionally synchronous inside the `ProjectGraph` root-build
transaction. The compiler is expected to perform graph-specific scheduling,
connection, temporal, storage, and background-evaluation topology analysis before
generating LLVM so the final LLVM program is already small/specialized enough
for a fast final optimization/codegen pass. Do not introduce an asynchronous
graph-JIT generation boundary merely to hide avoidable compiler work.

The executable project itself masquerades as an ordinary zero-input/zero-output
root node. Its generated declaration operation populates one `NodeLayoutBuilder`;
the resulting canonical `NodeLayout` covers realtime `State` plus
root/compiler-owned persistent regions needed by audio-thread execution. There is
no parallel graph-kernel realtime-storage arena. Background-only `TockState` and
background workspaces are executor-owned sidecars rather than part of the packed
`NodeStorage`.

The root has no Tock output ports and therefore no project-wide
`tock_coverage()`. Whole-project lowering partitions the background evaluation DAG into weakly connected
planning components, precomputes forward-change/reverse-demand/evaluation order,
and emits specialized component executors plus immutable port metadata.

The node port schema separates input access
(`SequentialInputConfig`/`RandomAccessInputConfig`), output production
(`TickOutputConfig`/`TockOutputConfig`), and `OutputRetention`. All concrete node
port schemas are static constexpr, including internal builder-created nodes; the
number and connectivity of their instances remain dynamic graph data. The checked-in source uses the independent access, production, and retention
schema above directly.

The existing node traits generate `tick_block()` from `tick()` when no native block
callback exists. A new explicit replayability type trait validates a restricted
pointwise subset (no `State`, random-access inputs, history or latency, and a
pure/deterministic fixed-version contract). GraphJit already imports the generated
block wrapper as LLVM; it may run that wrapper in the background evaluation
DAG when upstream data is available. The tock F/R callback API remains unchanged.

The direct connection validator checks each channel's capabilities rather than
requiring matching producer/consumer callback domains. A persisted tick output
provides random access over finalized published coverage; a contextually replayable
tick output provides it by background recomputation. A tock output can feed either
input access mode. Its output receives a transaction-local addressable materialization for a
downstream random-access input when ephemeral; persisted output uses retained
published storage. An **unreproducible ephemeral tick** source needs an
explicit authored recording bridge/node before random-access demand. Tiling retains
individual channel capabilities and adds no implicit recorder.

`tock_coverage()` and tock propagation/evaluation are background-only. Audio-thread
sequential inputs read an already published stale page as-is, or substitute their
own `neutral_value` when the required page is missing, without waiting. Persisted outputs never automatically evict generated/finalized covered data; the
author accepts potentially unbounded memory use. Coverage removal is the only
semantic reason to stop retaining it, while reader-pinned old storage versions
live until safe reclamation.

The recording bridge remains the explicit solution for unreproducible sequential
data. Realtime production uses capacity-manager-provisioned SPSC queue blocks and,
where layout permits, writes sample/event payloads directly into those blocks. A
completed producer-private chain is published to `BackgroundGraphExecutor` at a
realtime pass boundary. Background work independently pins one finite prefix from
each producer queue before evaluation; newly appended blocks are later work. Queue
publication is not persisted-state/page publication.

Persistent page width may continue to follow the fixed whole-graph root block
quantum as a storage layout choice. Changing root block size is a quiescent
storage-layout migration, not a semantic invalidation merely because the page
partition changed.

Static whole-project validation still computes semantic SCCs and rejects a
random-access input edge within a semantic SCC. The background evaluation DAG
also includes the sequential edges traversed by replayable tick producers and
must have no unresolved cycle; published persisted data can terminate traversal.
Tick-to-Sequential feedback retains its separate scheduling semantics.

Logical sample/event connections do not imply buffers. Connection implementation
selection is an explicit pure compiler-planning phase before LLVM generation; see
[sequential_port_storage_planning.md](./sequential_port_storage_planning.md).
Coverage, random-access, and background-evaluation semantics are described in
[coverage_and_background_evaluation.md](./coverage_and_background_evaluation.md).

See [graph_jit_direction.md](./graph_jit_direction.md) for the complete root-node,
`NodeLayout`/`NodeStorage`, ORC-lifetime, and lowering-boundary model.

## `RealtimeGraphExecutor` and `BackgroundGraphExecutor`

The runtime execution boundary is split between two peer application modules. There
is no parent `GraphExecutor` app module and no mutable state object shared by both
executors. The detailed handoff/queue contract is normative in
[realtime_background_execution_and_queues.md](./realtime_background_execution_and_queues.md).

### `RealtimeGraphExecutor`

`RealtimeGraphExecutor` owns the mutable realtime realization of an already compiled
project generation. It does not own ORC compilation or run background evaluation.
It keeps at least:

- one immutable active `CompiledGraph` generation and optionally one newest pending
  realtime generation;
- one canonical realtime `NodeStorage` for each retained executable generation;
- ordinary realtime lifecycle/migration state for `State` and safe whole-pass
  transition/steady activation;
- the active immutable persisted-state/page view selected for the current pass and
  optionally one newer pending version received from `BackgroundGraphExecutor`;
- address-stable Tick invocation/materialization bindings selected before a pass;
- producer endpoints for recording, Tick/persisted staging, node/background changes,
  or other dynamically accumulating data that must escape realtime execution; and
- realtime-visible failure/telemetry state for capacity exhaustion or missed
  mandatory production.

Receiving a compiled successor does not mutate an in-progress realtime pass. Staging
and any required non-realtime preparation complete first; activation occurs only at a
legal whole-pass boundary. Likewise, receiving a newer persisted-state version only
stores a pending immutable pointer. The current pass continues using its already
selected published state, and the pending version becomes active at a later pass
boundary.

Realtime-produced data is handed to background execution through independently
provisioned SPSC queues. The producer constructs a block chain privately, preferably
writing audio/events directly into provisioned blocks, and publishes the completed
chain at a pass boundary. The `BackgroundGraphExecutor` event handler only connects
that already initialized chain to the producer's pending queue; it does not
synchronously run background evaluation.

### `BackgroundGraphExecutor`

`BackgroundGraphExecutor` owns all mutable state for background evaluation and its
worker thread. It keeps at least:

- staged/current background `CompiledGraph` generations supplied by `ProjectGraph`,
  plus ordered prepared cutovers that have actually been published by realtime;
- separately owned lifecycle/storage for optional background-only `TockState`;
- stable canonical persisted-page stores/immutable roots for Tick/persisted and
  Tock/persisted outputs, with generation-specific bindings onto stable identities;
- committed per-generation background coverage state separated from reusable
  propagation/evaluation workspaces;
- transaction-local invocation/materialization frames and private page candidates;
- one independently pinnable pending queue per producer as required by recording,
  Tick/persisted staging, node/background changes, or later producer types;
- exact forward-change transactions, reverse-demand/tock transactions, and complete
  `tock/persisted` candidate completion; and
- immutable persisted-state/page versions that can be published back to realtime.

Incoming queue publications or control-plane updates only change pending/desired
input and may wake the worker. They do not alter work already selected by the worker.
Before executing, the worker independently pins one finite prefix from each relevant
producer queue and selects the exact immutable/versioned non-queue inputs for that
pass. There is intentionally no atomic snapshot relationship across different queues.
Items concurrently published during selection may belong to either the current or the
next background pass. If a future feature requires cross-queue atomic visibility, it
must stop and receive an explicit design rather than adding implicit global locking or
snapshot synchronization.

A `ProjectGraph`-staged successor generation is not ordinary immediately selectable
background desired state. The worker continues interpreting inputs under its current
generation until the corresponding realtime cutover has actually been published.

Once selected, a background workload is fixed. The worker performs coverage
propagation, Tock/replay work, materialization, persisted-page candidate construction,
and final validation against exactly that workload. Later queue appends and later
desired-state changes affect only subsequent work.

Final background commit is one logical publication boundary for the selected work:
prepared semantic coverage and, when present, the successor immutable persisted-page
snapshot advance together, or none advances. Completed queue prefixes are released
only after the corresponding work commits according to their domain semantics. The
worker may immediately select another workload when more pending input is already
available.

When a transaction produces a new coherent persisted-state/page version,
`BackgroundGraphExecutor` publishes one immutable heap object/pointer through its
bridge to `RealtimeGraphExecutor`. That asynchronous completion starts a new app-module
source invocation. The realtime handler stores the pointer as pending; it does not
synchronously affect the active pass.

### Paired-generation hot reload

One successful `GraphJit` result defines one logical graph generation with a realtime
half and a background half. `ProjectGraph` stages the background half first. Background
staging allocates/prepares generation-specific queues, stable-identity/route bindings,
and an allocation-free cutover publication object/reference. Only after that
preparation succeeds does `ProjectGraph` stage the corresponding realtime half.

Staging does not activate either half. The authoritative generation cutover happens at
a later realtime pass boundary. `RealtimeGraphExecutor` first publishes every final
old-generation producer chain completed by the pass, then synchronously publishes the
already-prepared `N -> N+1` cutover to `BackgroundGraphExecutor`, then swaps its active
realtime realization. The background-side handler only links/publishes that prepared
cutover for its worker and returns; it performs no dynamic allocation or background
evaluation.

The background worker may lag. Work already selected under generation N remains
immutable and completes under N. The cutover closes generation-N producer endpoints,
so their queues have finite tails. Background drains/finalizes remaining N work,
performs the prepared N->N+1 persisted-state migration, and only then interprets N+1
queued data under the N+1 graph. There is no global atomic snapshot across queues.

This makes replacement order semantically irrelevant after staging. Old-generation
data is always interpreted using old-generation routes. A disappeared producer simply
stops producing after the boundary and does not erase surviving recorded state. A
disappeared destination accepts its remaining old-generation work before being retired
during migration. Stable logical identity, never generation-local slot/index reuse,
determines which retained data survives.

Persisted-state versions published back to realtime identify the generation with which
they are compatible. A final N result completed after realtime switched to N+1 is an
input to the background N->N+1 transition, not a version that may become active in the
N+1 realtime graph. Compatible versions are stored pending by realtime and applied only
at legal pass boundaries. A successor that was merely staged but never activated may
be superseded without creating a semantic generation boundary. Once realtime actually
cuts over, however, that cutover must remain ordered while background lags and cannot
be collapsed to a latest-generation pointer.

See
[realtime_background_execution_and_queues.md](./realtime_background_execution_and_queues.md)
for the normative cutover and ownership protocol.

### Provisioned queues and capacity management

The dynamically sized cross-thread queues are runtime infrastructure, not shared
executor state. Each queue has one producer and one consumer, uses power-of-two
fixed-capacity blocks, and is provisioned ahead of producer demand by a non-app-module
`AsyncCapacityManager` (provisional name).

Only the producer advertises its capacity requirement:

```text
C = maximum producer burst that must fit without provisioning
L = low free-capacity watermark
H = refill target
G = allocation/segment granularity
```

`C` is a producer-derived structural bound used to derive/validate policy; steady
maintenance primarily uses `L/H/G`. The intended relationship remains `C << L < H`.
The capacity manager allocates/recycles off the realtime path and may keep ready blocks
preassigned to each producer. Queue users do not require one shared atomic logical
size.

Block ownership follows one direction:

```text
AsyncCapacityManager -> producer -> BackgroundGraphExecutor -> AsyncCapacityManager
```

The producer owns and initializes a private block/chain before publication. The
consumer owns published blocks until the selected prefix is committed/released. The
consumer does not splice released blocks directly back into a realtime producer's
reserve; reclamation/provisioning returns through the capacity manager.

The queue's consumer-side insertion is intentionally cheap. An incoming fully built
chain is attached by one publication/pointer operation (or installs the first pointer
when the queue was empty). Background workload selection discovers and remembers the
currently visible terminal block before execution; later links after that block are
next-pass work.

### Persisted data and Random Access

Dynamically sized persisted output data is deliberately not part of fixed
`NodeStorage`. Stable persisted outputs are not owned by one JIT generation merely
because port indices are generation-local: compatible generations rebind stable
virtual-node/member/output identities to the same background-owned canonical page
store without copying data. Ephemeral outputs own no persisted data.

Tick/persisted production reaches that same canonical store through the provisioned
queue handoff rather than a Tock callback. Explicit recording uses the same queue/
capacity infrastructure but its fixed RAM overwrite semantics remain distinct from
Tick/persisted page retention. Queue blocks are pre-publication/background inputs,
not a second Random Access retained-data representation.

For recording, ordinary writes overwrite the addressed RAM recording, untouched
outputs enqueue nothing and preserve previous content, and `write_void()` enqueues an
explicit authoritative erase. Resource exhaustion never authorizes intentional loss
of a written recording block.

Executable-generation reconciliation treats genuinely new semantic nodes as node
creation events. Outputs participating in background coverage propagation establish
exact coverage through authored or compiler-generated forward-coverage semantics;
Tock/persisted candidates become publishable only after full materialization.

Changing the connection set of a random-access input conservatively marks that whole
logical input changed over `old_input_coverage | new_input_coverage`; ordinary forward
propagation determines downstream effects. Reverse coverage planning is value-blind
and may conservatively request a larger input region when dependency addressing
depends on input data values.

The realtime thread selects one immutable published page/materialization view at the
pass boundary, plays present pages even when out of date, and substitutes the
consuming Sequential input's `neutral_value` for missing pages. Tick-time Random
Access uses the same selected immutable state. It never invokes Tock, allocates on a
page miss, or waits for background replacement. Newly queued realtime blocks and
newly published background versions do not become visible mid-pass.

Persisted output data remains logically retained throughout generated coverage: there
is no eviction merely for memory pressure, invalidation, or lack of current readers.
Old immutable storage versions are released only when their reader pins disappear;
coverage removal is the semantic reason to stop retaining the data.

Background evaluation uses a coherent selected semantic/page base for each workload.
A newer `tock/persisted` candidate is pending until the entire stored output required
by that version pair is complete; callers may continue displaying an older completed
pair rather than observe partial/default data. `tock/ephemeral` requests evaluate
exact requested coverage against one selected immutable version pair.

The generated background root owns static node order and calls only narrow
transaction-local prepare/finalize hooks selected by the compiled plan; generated
code never receives either executor pointer, the page store, the queue/capacity
manager, or a transaction-owner pointer.

Reader-pin and retired-owner reclamation use audio-safe boundary/epoch ownership. The
realtime thread does not allocate, block, or synchronously destroy the final retired
owner. Queue blocks and retired immutable versions return to non-realtime reclamation.

Changing project sample rate invalidates/repropagates background-computed output
semantics. Tick/persisted samples are not automatically resampled or remapped to new
sample indices; an explicit sampler/resampler node preserves original timing when
required.

If synchronous `GraphJit` compilation fails, `ProjectGraph` retains the desired
revision and diagnostics and sends no partial successor to either executor. The
previous active realtime executable generation and previous valid background/persisted
state may continue. Desired project revision, active realtime executable revision,
and background desired/committed revision are therefore distinct state.

## System audio devices

System audio device nodes are ordinary project nodes configured with a stable
value object identifying the logical device, for example:

```text
"default"
"some-backend-stable-device-id"
```

`"default"` is simply another stable identity that may be persisted in project
state. Code outside `SystemAudioDevices` does not special-case it.

`SystemAudioDevices` must return a stable logical binding for every syntactically
valid requested device id even when no hardware device currently resolves to
that id.

When unavailable:

- an input-device binding produces silence;
- an output-device binding accepts/discards provided blocks.

The same logical binding may transparently begin or stop communicating with a
hardware device as hardware availability changes. Generated nodes therefore do
not hold a fragile reference directly to a volatile hardware device.

A node may resolve its logical binding in `Node::initialize()` directly or via
an appropriate linker-set query keyed by the stable id value object. The
binding contract itself must tolerate hardware-device disappearance at any
point during graph execution.

Initially, users create/manage system audio device nodes manually through the
normal project-node interface. Automatic creation/removal of one node per
detected device is an optional future convenience service, not the foundation
of device support.

See [system_audio_devices_direction.md](./system_audio_devices_direction.md).

## Persistence

`ProjectPersistence` is a serializer/replayer, not the canonical project graph
owner.

Persistent graph state should include the normalized user-owned state necessary
to reconstruct `ProjectGraph`, including:

- stable node-instance ids;
- definition ids;
- restricted configuration argument-list source;
- project-wide connections expressed with `ProjectNodePortMatcher`s;
- explicit connection channel types;
- other project-owned metadata as it becomes part of the canonical graph model.

Do not persist:

- `NodeInstances` cache entries;
- parsed configuration-expression cache and retained typed values;
- embedding maps;
- builder-local handles;
- resolved concrete connection ids;
- `GraphJit` compiled generations/ORC resources;
- `RealtimeGraphExecutor` / `BackgroundGraphExecutor` runtime storage or execution caches;
- volatile hardware audio-device objects.

Project replay may produce unresolved node instances/connections until package
loading completes. That is a valid initialized state.

## Presentations

The initial project-graph work does not require a generic automatically-managed
subgraph/controller layer.

A future presentation system may use:

- `PresentationDefinitions` for available presentation kinds/providers; and
- `PresentationInstances` for live presentation state and presentation-owned
  graph requests.

A presentation that needs structural graph changes should route those changes
through the same `ProjectGraph` root-build transaction rather than reaching
`NodeInstances` and `GraphConnections` through a second converging path. A purely
visual presentation need not own graph state at all.

`PresentationInstances` may later publish asynchronous UI updates to
`SocketRpcServer` so webviews do not poll. Such a notification is a separate
source/cause from an incoming JSON-RPC request: `SocketRpcServer` may be a root
in the request tree and a child/sink in the notification tree, but the same
cause must never leave and then re-enter it.

Detailed presentation event flows are deferred until the core execution graph
is implemented. The intended follow-on presentation/manual-control semantics are
now consolidated in
[node_presentation_and_manual_controls_direction.md](./node_presentation_and_manual_controls_direction.md):
one `NodePresentation` extension family, four global display modes, stable
interaction rebinding, persistent manual-input values, scrub-only interpolation,
and code-only dynamic-input specialization over one compatible `NodeLayout`.

## Immediate implementation order

The implementation checkpoints now stand as follows:

1. **Landed:** generalized `NodeDefinitions` plus the package pipeline that feeds
   one immutable accepted-definition snapshot into it.
2. **Landed:** direct frozen `ConfiguredGraph` embedding through the same
   importer as live children, with explicit local-to-parent handle translation
   and scope-preserving virtual-node import.
3. **Landed (configuration core):** generalized/renamed `NodeInstances`,
   provider-generated typed owned argument operations, one-snapshot recursive
   configuration, batched diagnostics, and reusable frozen-graph value caching.
   Cache invalidation is deliberately whole-snapshot for now.
4. **Pending independent NodeInstances checkpoint:** parse/type-check/evaluate persisted
   restricted configuration argument-list source into owned typed argument tuples plus
   the generated operations needed by the existing `NodeInstances` value-level cache path.
5. **Landed:** `ProjectGraph` is the root-build transaction coordinator without
   duplicating the desired instance/connection sets.
6. **Landed (semantic connection core):** `GraphConnections` owns desired
   cross-node connection intent, resolves recursive structured
   `ProjectNodePortMatcher`s against the complete placement map, applies
   sample/event connections, and preserves dangling matchers with diagnostics.
   Structured persistence and JSON-RPC adapters remain follow-up transport work.
7. **Landed, with a planned storage split (fixed state + existing
   `BackgroundEvaluationPlan` foundation):** the current implementation still
   places non-semantic tock-only `TockState` in canonical `NodeStorage` alongside
   compiler-owned raw aligned regions. The target contract moves `TockState` to
   separately owned background storage so `NodeStorage` remains the packed
   audio-thread realization. The current source independently represents output
   retention and retains stable `BackgroundEvaluationPlan` topology metadata. The final
   access/production schema names, replayability trait, background-only tock,
   background evaluation DAG, and new capture/publication execution are
   **target work, not landed**.
   The detailed dependency order is normative in
   [coverage_and_background_evaluation.md §32](./coverage_and_background_evaluation.md#32-implementation-landing-order).
8. **Landed (compiler shell + storage/lifecycle ABI cleanup):** `GraphJit`
   synchronously captures exact package LLVM/provenance, resolves compiler
   anchors/config relocations, verifies and O3 optimizes generated project LLVM,
   owns the project ORC domain, and returns independently releasable
   `CompiledGraph` generations carrying the canonical `NodeLayout` plus the generated
   root `tick_block`; primitive `skip_block` callbacks remain internal scheduler
   operations and are not exposed as a root ABI;
9. **Landed:** the legacy `GraphLowerer`/`GraphCompiler`/`RuntimeGraphRoot`
   execution path and non-constexpr concrete-port fallbacks are deleted, and
   source introspection is derived from `ConfiguredGraph`; next migrate the port
   schema and replayability/connection planning;
10. complete ordinary background evaluation in the normative dependency order from
    [coverage_and_background_evaluation.md §32](./coverage_and_background_evaluation.md#32-implementation-landing-order):
    the committed-coverage/propagation-workspace split and compiler-owned runtime
    binding/materialization/replay schedules have landed. The executor-level canonical
    sample/event page store has also landed with private structurally shared candidates,
    semantic/page coordinates, whole-root stale-base publication, pre-registered
    non-owning reader pins and explicit non-audio retired-root reclamation.
    `BackgroundStorageRealization` now owns address-stable sparse transaction buffers,
    binds published pages through the transaction's non-owning pinned view, resolves
    stable/generation-local persisted identity and validates explicit views before
    sealing. It now executes compiler-planned direct/sample/event operations exactly
    once over those sealed views, including latency-aware heterogeneous sample
    projection and shared stable event conversion/fan-in. The generated evaluate root
    now invokes narrow transaction-local prepare/finalize hooks around each active
    authored-Tock call or complete replay loop. Those hooks receive only an opaque
    `BackgroundStorageOperationFrame` containing the realization and compiler-owned
    before/after spans; forward/reverse propagation remains hook-free. The transaction
    coordinator now populates authored-Tock/replay bindings, allocates isolated replay
    storage, completes persisted-page demand from one pinned base, invokes the generated
    evaluate root once, and promotes prepared coverage only after any required private
    page publication succeeds. It also validates produced-sample completeness before
    staging pages and limits each multi-output replay flush to that output's selected
    coverage. Failed, incomplete or stale transactions promote neither state, and
    page-free operations neither advance the page version nor skip pinned-base stale
    validation. The Tick binding frame, callback-lifetime published-root pin and
    compiler-planned direct persisted-page Random Access sample/event views have now
    landed. Their address-stable workspace is allocated on the control path and the
    generated root receives only compact resolved spans. Background-produced derived
    and ephemeral Random Access views now cross the root boundary in a separately
    pinned immutable `TickMaterializationSnapshot`; generation/page-version matching
    prevents incoherent combinations, and retired owners are reclaimed explicitly off
    the audio thread. Next add compiler-planned sequential snapshot playback and
    per-input missing-page neutrality. Do this before enabling transactional recording
    consumption;
11. integrate stable logical `SystemAudioDevices` bindings with ordinary system
    audio leaf node definitions;
12. once GraphJit plus `RealtimeGraphExecutor` and `BackgroundGraphExecutor` have fully landed as the normal execution
    path, run a substantial optimization/profiling iteration over the complete
    runtime plus the current configuration/cache/builder path and establish the
    performance baseline that later structural work must preserve;
13. make the scoped `BuilderSession`/`GraphBuilder` API migration in
    [scoped_graph_builder_and_subgraph_closure_direction.md](./scoped_graph_builder_and_subgraph_closure_direction.md)
    the next builder-level architectural step after that optimization pass;
14. add presentation/manual-control and automatic-device convenience services
    only after the core graph path is stable, using
    [node_presentation_and_manual_controls_direction.md](./node_presentation_and_manual_controls_direction.md)
    for the presentation/control contract.
