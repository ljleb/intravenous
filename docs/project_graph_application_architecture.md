# Project Graph Application Architecture

_Status: current design direction for the next project-graph implementation phase._

This document is the authoritative application-module design for the unified
project graph after the lane/timeline/task-runner deletion checkpoint. Where it
conflicts with older lane-oriented or iv-module-specific notes, this document
wins.

Related documents:

- [event_propagation_tree_constraint.md](./event_propagation_tree_constraint.md)
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

- `NodeDefinitions`
- `NodeInstances`
- `PackageReload`
- `PackageDefinitions`
- `NodeSourceIntrospection` or a later shorter name if its responsibility
  becomes broader than node source introspection

IV-specific names remain appropriate where they identify an actual source
format, registration API, package ABI, or compiler concept.

## App-module inventory

The core project-graph modules are:

| Module | Primary responsibility |
| --- | --- |
| `PackageReload` | detect/build/reload IV packages and publish completed package-provider updates |
| `PackageDefinitions` | own configured/known package catalog state and package-level user operations |
| `NodeDefinitions` | own the current immutable, versioned node-definition registry snapshot |
| `ProjectGraph` | own durable user graph intent and orchestrate one complete root-graph configuration transaction |
| `NodeInstances` | instantiate one requested batch against exactly one definitions snapshot; own reusable configured node-instance caches |
| `GraphConnections` | resolve project-wide port matchers against one complete root embedding and apply cross-node connections |
| `GraphJit` | synchronously lower, optimize, and ORC-JIT one complete root `ConfiguredGraph` into an immutable `CompiledGraph` generation |
| `GraphExecutor` | own active/pending compiled generations, mutable node storage, execution requests, state migration, and safe-boundary activation |
| `NodeSourceIntrospection` | derived read model for source/logical-node/tooling queries; provisional generalized name |
| `SystemAudioDevices` | own system-audio enumeration, logical device bindings, physical-device lifetime, buffering, and synchronization |
| `ProjectPersistence` | load/save normalized persistent state without becoming the canonical graph owner |
| `ProjectAutosave` | coalesce configured-state mutations and request persistence |
| `SocketRpcServer` | transport JSON-RPC requests/notifications without becoming the owner of project state |

Future presentation modules may include `PresentationDefinitions` and
`PresentationInstances`. They are intentionally not required for the initial
project-graph implementation.

## `ProjectGraph` combines state ownership and root-graph orchestration

There is no separate `RootGraph` app module.

`ProjectGraph` coherently owns both:

- canonical user-authored project graph intent; and
- the transaction that materializes that intent into one root
  `ConfiguredGraph`.

Its durable graph state initially contains:

- user-created node-instance declarations;
- user-created project-wide connection declarations; and
- a monotonically increasing project revision.

Its root-build procedure is always batched:

1. create one fresh root `GraphBuilder`;
2. invoke `NodeInstances` exactly once to resolve/embed the complete requested
   instance batch;
3. invoke `GraphConnections` exactly once after all requested instances have
   been considered, passing the same root builder and the complete embedding
   map;
4. finish the root builder into one `ConfiguredGraph`;
5. invoke `GraphJit` exactly once to synchronously compile that graph into one
   immutable `CompiledGraph`;
6. invoke `GraphExecutor` exactly once with that compiled successor generation.

The four downstream modules are siblings in the propagation tree. Their numeric
order above is execution order inside one `ProjectGraph` handler, not a
parent/child relationship between those modules.

A project mutation can be accepted even if some node definitions are currently
unavailable or some configuration expressions fail to compile. Desired project
state is durable; its current configured/runtime realization may be incomplete
and should carry diagnostics rather than deleting user intent.

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
- [Package reload](./event_flows/package_reload.md)
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

`NodeInstances` does not own the canonical desired project instance list. It
receives the complete requested batch for the current root-build transaction
from `ProjectGraph`. Its retained state is cache/configuration machinery and any
derived diagnostics needed to make later batches efficient.

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

`GraphConnections` owns project-wide cross-node connection **resolution and
application** for one complete requested batch. Canonical desired connection
declarations remain owned by `ProjectGraph`; `GraphConnections` may retain only
derived resolution/cache/diagnostic state that is useful across transactions.

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
connection, temporal, storage, and lifecycle analysis before generating LLVM so
the final LLVM program is already small/specialized enough for a fast final
optimization/codegen pass. Do not introduce an asynchronous graph-JIT generation
boundary merely to hide avoidable compiler work.

Logical sample/event connections do not imply buffers. Connection implementation
selection is an explicit pure compiler-planning phase before LLVM generation;
see [realtime_port_storage_planning.md](./realtime_port_storage_planning.md).

See [graph_jit_direction.md](./graph_jit_direction.md) for ORC ownership,
`CompiledGraph` lifetime, and the two-JIT compiler model.

## `GraphExecutor`

`GraphExecutor` owns the mutable runtime realization of an already compiled
project generation. It does not own ORC compilation.

`GraphExecutor` keeps at least:

- one immutable active `CompiledGraph` generation;
- optionally one newest pending compiled generation;
- live `NodeStorage` and pass-scoped execution state/resources;
- state correspondence/migration information needed to activate a successor;
- sequential execution and compiled sample/event request handling against the
  active generation.

Receiving a new `CompiledGraph` does not mutate an in-progress audio pass. Work
that is safe before the boundary may be prepared immediately, but replacement
or modification of active execution occurs only after a complete pass has
finished.

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

The most useful order is:

1. generalize/rename the definition registry to `NodeDefinitions` and make its
   immutable snapshot sufficient for recursive construction;
2. implement typed owned configuration argument operations and C++ expression
   thunk compilation;
3. repair `GraphBuilder`/`ConfiguredGraph` embedding, recursive scopes, stable
   handle translation, and tiled-child identity;
4. generalize/rename `NodeInstances` and implement one-snapshot batched
   configuration plus caching;
5. introduce `ProjectGraph` with durable node declarations and one root-builder
   transaction;
6. introduce `GraphConnections` and recursive `ProjectNodePortMatcher`
   resolution;
7. introduce pure connection/history/latency/event-window storage planning;
8. introduce `GraphJit` with synchronous whole-project LLVM/ORC compilation;
9. introduce `GraphExecutor` ownership of runtime storage, execution requests,
   state migration, and safe-boundary activation;
10. integrate stable logical `SystemAudioDevices` bindings with ordinary system
    audio leaf node definitions;
11. add presentation-specific and automatic-device convenience services only
    after the core graph path is stable.
