# Project Graph Application Architecture

_Status: current project-graph architecture and implementation direction._

This document is the authoritative application-module design for the unified
project graph after the lane/timeline/task-runner deletion checkpoint. Where it
conflicts with older lane-oriented or iv-module-specific notes, this document
wins.

Related documents:

- [event_propagation_tree_constraint.md](./event_propagation_tree_constraint.md)
- [package_pipeline_architecture.md](./package_pipeline_architecture.md)
- [node_definitions_and_instances_direction.md](./node_definitions_and_instances_direction.md)
- [graph_builder_embedding_and_matchers.md](./graph_builder_embedding_and_matchers.md)
- [system_audio_devices_direction.md](./system_audio_devices_direction.md)
- [graph_jit_direction.md](./graph_jit_direction.md)
- [realtime_port_storage_planning.md](./realtime_port_storage_planning.md)
- [startup_realization_order.md](./startup_realization_order.md)
- [unified_graph_direction.md](./unified_graph_direction.md)
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
| `GraphExecutor` | own active/pending compiled generations, mutable node storage, execution requests, state migration, and safe-boundary activation |
| `IvModuleSourceIntrospection` | derived source/logical-node read model for module nodes only |
| `SystemAudioDevices` | own system-audio enumeration, stable logical device bindings, physical-device lifetime, buffering, and synchronization |
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

The dependency watcher already uses `inotify`. The remaining periodic
package-root discovery scan in `PackageWatcherService` is explicitly temporary
and should be replaced by event-driven Linux discovery rather than preserved as a
portability fallback.

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
callbacks and declaring compiler-owned raw regions; `GraphExecutor` owns the
corresponding `NodeStorage`; and internal indexed outputs are reached through
specialized indexed-component metadata rather than a synthetic project-root
`tock_coverage()`. Final layout offsets are therefore compile-time constants in
the generated LLVM. `GraphExecutor` remains
unimplemented. Structured connection persistence/JSON-RPC adapters are also still
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
6. invoke `GraphExecutor` exactly once with that compiled successor generation.

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
`GraphConnections`, `GraphJit`, and `GraphExecutor` for every execution-affecting
graph change.

Batches are the normal API shape. One logical graph change must not emit one
application event per node or per connection.

The fundamental event procedures are documented separately:

- [User node mutation](./event_flows/user_node_mutation.md)
- [Package refresh](./event_flows/package_refresh.md)
- [User connection mutation](./event_flows/user_connection_mutation.md)
- [Startup and project replay](./event_flows/startup_and_project_replay.md)

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

Project-authored configuration arguments are persisted initially as one C++
argument-list source string supplied by the UI, for example:

```cpp
OscillatorConfig{.frequency = 440.0f}, 0.25f
```

Do **not** split this string on commas in application code. Valid C++ argument
expressions may themselves contain commas in calls, braced initialization,
templates, lambdas, and other syntax. The whole argument-list source is handed
to Clang.

The argument-list source is compiled in provider/callee translation-unit
context so the actual demanded C++ argument types are known. A generated thunk
can materialize typed globals/owned values and invoke the existing erased
configuration ABI.

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

The C++ source string can key the expression-thunk compilation cache. The
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
C++ configuration argument-list source
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
builder-local `NodeRef`s and not a newly invented "endpoint" vocabulary.

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
connection, temporal, storage, and indexed-topology analysis before
generating LLVM so the final LLVM program is already small/specialized enough
for a fast final optimization/codegen pass. Do not introduce an asynchronous
graph-JIT generation boundary merely to hide avoidable compiler work.

The executable project itself masquerades as an ordinary zero-input/zero-output
root node. Its generated declaration operation populates one `NodeLayoutBuilder`;
the resulting canonical `NodeLayout` covers normal `State`, optional tock-only
non-semantic `IndexedState`, and root/compiler-owned persistent or bounded reusable
regions. There is no parallel graph-kernel storage arena.

The root has no indexed outputs and therefore no project-wide `tock_coverage()`.
Whole-project lowering partitions the indexed subgraph into weakly connected
planning components, precomputes forward-change/reverse-demand/evaluation order,
and emits specialized component executors plus immutable endpoint metadata.

The **target** node port schema separates input access
(`SequentialInputConfig`/`RandomAccessInputConfig`), output production
(`TickOutputConfig`/`TockOutputConfig`), and `OutputRetention`. All concrete node
port schemas are static constexpr, including internal builder-created nodes; the
number and connectivity of their instances remain dynamic graph data. The checked-
in source's older `IndexedProducer` API is not yet migrated by this documentation.

The existing node traits generate `tick_block()` from `tick()` when no native block
callback exists. A new explicit replayability type trait validates a restricted
pointwise subset (no `State`, random-access inputs, history or latency, and a
pure/deterministic fixed-version contract). GraphJit already imports the generated
block wrapper as LLVM; it may run that wrapper in the background random-access
DAG when upstream data is available. The tock F/R callback API remains unchanged.

The direct connection validator checks each channel's capabilities rather than
requiring matching producer/consumer callback domains. A persisted tick output
provides random access over finalized published coverage; a contextually replayable
tick output provides it by background recomputation. A tock output can feed either
input access mode. Its output is page-materialized for a downstream random-access
input, even when ephemeral. An **unreproducible ephemeral tick** source needs an
explicit authored recording-policy node before random-access demand. Tiling retains
individual channel capabilities and adds no implicit recorder.

`tock_coverage()` and tock propagation/evaluation are background-only. Audio-thread
sequential inputs read an already published stale page as-is, or substitute their
own `neutral_value` when the required page is missing, without waiting. Persisted
outputs never automatically evict generated covered pages; the author accepts
potentially unbounded memory use. Pages leave logical storage only if output
coverage removes them, while reader-pinned old physical versions live until safe
reclamation.

The recording bridge remains the explicit solution for unreproducible sequential
data. It captures produced blocks into allocator-provisioned slabs; a background
pass consumes a fixed capture-sequence prefix through F/R/evaluation and publishes
one complete successor. Capture insertion is not page publication.

Persistent page width may continue to follow the fixed whole-graph root block
quantum as a physical layout choice. Changing root block size is a quiescent
physical-layout migration, not a semantic invalidation merely because the page
partition changed.

Static whole-project validation still computes semantic SCCs and rejects a
random-access input edge within a semantic SCC. The background evaluation DAG
also includes the sequential edges traversed by replayable tick producers and
must have no unresolved cycle; published persisted data can terminate traversal.
Sequential feedback retains its separate realtime scheduling semantics.

Logical sample/event connections do not imply buffers. Connection implementation
selection is an explicit pure compiler-planning phase before LLVM generation; see
[realtime_port_storage_planning.md](./realtime_port_storage_planning.md).
Indexed-access semantics and static planning are described in
[indexed_dsp_nodes.md](./indexed_dsp_nodes.md).

See [graph_jit_direction.md](./graph_jit_direction.md) for the complete root-node,
`NodeLayout`/`NodeStorage`, ORC-lifetime, and lowering-boundary model.

## `GraphExecutor`

`GraphExecutor` owns the mutable runtime realization of an already compiled
project generation. It does not own ORC compilation.

`GraphExecutor` keeps at least:

- one immutable active `CompiledGraph` generation and optionally one newest
  pending generation;
- one canonical `NodeStorage` for each retained executable generation;
- ordinary lifecycle/migration state for `State` and optional tock-only
  `IndexedState`;
- stable persistent indexed stores/immutable roots for identifiable `tock/persisted`
  outputs, with per-generation endpoint bindings;
- reusable indexed transaction workspace for reverse/forward planning and
  non-realtime `tock/ephemeral` materialization;
- indexed semantic versions plus monotonically advancing immutable pages versions
  and candidate/published snapshots;
- the recording-capture log, processed-sequence frontier, slab allocator/
  reclamation state, and page-version reader pins;
- exact forward-change transactions, reverse-demand/tock transactions, and full
  `tock/persisted` candidate completion;
- sequential execution through the generated zero-port root node; and
- versioned external indexed sample/event requests/change notifications.

Dynamically sized tock/persisted output data is deliberately not part of fixed
`NodeStorage`. Stable tock/persisted outputs are not owned by one JIT generation
merely because endpoint ordinals are generation-local: compatible generations rebind
stable virtual-node/member/output identities to the same executor-owned storage
without copying payloads. `tock/ephemeral` owns no persistent output payload.
Finalized tick/persisted content is a directly readable random-access stored
boundary, not a tock callback. Recording bridges own separate capture storage
whose records are transaction inputs
to the bridge's indexed output. Published indexed versions materialize their own
payload and never retain pointers into capture blocks.

Executable-generation reconciliation treats genuinely new semantic nodes as node
creation events. Computed indexed outputs establish exact coverage through mandatory
forward-coverage semantics; tock/persisted candidates become publishable only
after full materialization. Recording captures seed changed/added coverage on the
explicit bridge's indexed output only inside a fixed indexed transaction. The pages
version advances when that complete propagation/tock transaction commits; JIT
compilation alone is not an indexed invalidation event.

Changing the connection set of an indexed input conservatively marks that whole
logical input changed over `old_input_coverage | new_input_coverage`; ordinary
forward propagation determines downstream effects. Reverse coverage planning is
value-blind and may conservatively request a larger input region when dependency
addressing depends on input payload values.

Receiving a new `CompiledGraph` does not mutate an in-progress audio pass.
The audio thread pins one published page view, plays present pages even when
out of date, and substitutes the consuming sequential input's `neutral_value`
for missing pages. It never invokes tock, allocates on a page miss, or waits
for its replacement. Published tock/persisted data remains immutable. Recording capture storage is
separate from published pages: the realtime path appends immutable captured blocks,
while an indexed pass uses only the capture-sequence prefix fixed at its start.
Later captures cannot enter that pass through the allocator/capture object.

Persisted output pages remain logically retained throughout generated coverage:
no eviction for memory pressure, invalidation or lack of current readers. Old
immutable physical versions are released only when their reader pins disappear;
coverage removal is the only reason to drop a logical retained page.

Non-realtime indexed results are versioned by `(semantic_version, pages_version)`.
A newer `tock/persisted` candidate is pending until the **entire** stored output
required by that version pair is complete; callers may continue displaying an older
completed pair rather than observe partial/default data. `tock/ephemeral`
requests evaluate exact requested coverage against one selected immutable version
pair.

Changing project sample rate invalidates/repropagates computed indexed semantics.
Realtime/persisted samples are not automatically resampled or reindexed;
they are interpreted at the new project rate unless an explicit sampler/resampler
node preserves original timing.

The mechanism intentionally preserves the useful part of the deleted
`TasksRunner` update model without preserving task-graph or lane semantics.

If synchronous `GraphJit` compilation fails, `ProjectGraph` retains the desired
revision and diagnostics but does not invoke `GraphExecutor` with a partial
successor. The previous active executable generation may continue running.
Desired project revision and active executable revision are therefore distinct
state even though graph compilation itself is synchronous.

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
valid requested device id even when no physical device currently resolves to
that id.

When unavailable:

- an input-device binding produces silence;
- an output-device binding accepts/discards provided blocks.

The same logical binding may transparently begin or stop communicating with a
physical device as hardware availability changes. Generated nodes therefore do
not hold a fragile reference directly to a volatile physical device.

A node may resolve its logical binding in `Node::initialize()` directly or via
an appropriate linker-set query keyed by the stable id value object. The
binding contract itself must tolerate physical-device disappearance at any
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
- C++ configuration argument-list source;
- project-wide connections expressed with `ProjectNodePortMatcher`s;
- explicit connection channel types;
- other project-owned metadata as it becomes part of the canonical graph model.

Do not persist:

- `NodeInstances` cache entries;
- compiled expression thunks;
- embedding maps;
- builder-local handles;
- resolved concrete connection ids;
- `GraphJit` compiled generations/ORC resources;
- `GraphExecutor` runtime storage or execution caches;
- volatile physical audio-device objects.

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
is implemented.

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
4. **Pending independent NodeInstances checkpoint:** compile persisted C++
   configuration argument-list source into owned typed expression thunks/tuples
   and feed them into the existing `NodeInstances` value-level cache path.
5. **Landed:** `ProjectGraph` is the root-build transaction coordinator without
   duplicating the desired instance/connection sets.
6. **Landed (semantic connection core):** `GraphConnections` owns desired
   cross-node connection intent, resolves recursive structured
   `ProjectNodePortMatcher`s against the complete placement map, applies
   sample/event connections, and preserves dangling matchers with diagnostics.
   Structured persistence and JSON-RPC adapters remain follow-up transport work.
7. **Landed (fixed state + existing indexed-plan foundation):** canonical
   `NodeLayout`/`NodeStorage` supports non-semantic tock-only `IndexedState`
   and compiler-owned raw aligned regions; the current source retains the old
   `IndexedProducer` and recorder-staging planning. The independent port schema,
   replayability trait, background-only tock, generalized random-access DAG,
   and new capture/publication execution are **target work, not landed**.
   The detailed dependency order is normative in
   [indexed_dsp_nodes.md §32](./indexed_dsp_nodes.md#32-implementation-landing-order).
8. **Landed (compiler shell + storage/lifecycle ABI cleanup):** `GraphJit`
   synchronously captures exact package LLVM/provenance, resolves compiler
   anchors/config relocations, verifies and O3 optimizes generated project LLVM,
   owns the project ORC domain, and returns independently releasable
   `CompiledGraph` generations carrying the canonical `NodeLayout` plus the generated
   root `tick_block`; primitive `skip_block` callbacks remain internal scheduler
   operations and are not exposed as a root ABI;
9. first remove the legacy `GraphLowerer`/`GraphCompiler`/`RuntimeGraphRoot`
   execution path and non-constexpr concrete-port fallbacks; move source
   introspection onto `ConfiguredGraph`, then migrate the port schema and
   replayability/connection planning;
10. build the reusable indexed batch ABI, generated F/R/background evaluation,
    and `GraphExecutor` publication before enabling transactional recording
    consumption; add stale-page playback and per-input missing-page neutrality;
11. integrate stable logical `SystemAudioDevices` bindings with ordinary system
    audio leaf node definitions;
12. add presentation-specific and automatic-device convenience services only
    after the core graph path is stable.
