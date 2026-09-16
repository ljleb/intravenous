# Node Definitions And Instances Direction

_Status: current detailed design for generalized node definition lookup,
configuration evaluation, and reusable node-instance caching._

This document specializes the responsibilities described in
[project_graph_application_architecture.md](./project_graph_application_architecture.md).
It supersedes iv-module-only instance-management assumptions in
[iv_module_instance_management_direction.md](./historical/iv_module_instance_management_direction.md)
and [iv_module_instances_graph_input_direction.md](./historical/iv_module_instances_graph_input_direction.md).

## Implementation checkpoint: `NodeDefinitions`

The first generalized app-module checkpoint is implemented as `NodeDefinitions`.
The former `IvModuleDefinitions` runtime module has been renamed and now publishes
an immutable `NodeDefinitionsSnapshot` that unifies leaf and module definitions in
one stable-ID namespace.

Each published entry carries a per-provider version, the construction/signature
surface required for later invocation, and lifetime references that pin the loaded
provider revision. Republishing one package advances the versions of definitions
owned by that package while unchanged definitions owned by other packages retain
their versions. The registry snapshot generation advances only when the published
definition set/provider revisions actually change.

The currently retained construction surface includes module/leaf configuration
callbacks plus the registered signature callback. The typed owned-argument
operations described below are intentionally deferred to the `NodeInstances`
checkpoint, where their concrete cache requirements become executable rather than
being speculative registry API.

Existing iv-module-era consumers temporarily continue to receive the compatibility
package-definition diff event. Future batched configuration must consume one
immutable definitions snapshot and must not re-enter `NodeDefinitions`.

## Definitions are providers, not configured instances

`NodeDefinitions` owns one immutable versioned registry containing both leaf
node definitions and module node definitions.

A definition entry identifies how to construct/configure one node from typed
configuration arguments. It is not itself a `ConfiguredGraph`, and it does not
own the cache of configured invocation results.

The snapshot must be complete enough that another app module can perform
recursive configuration without entering `NodeDefinitions` again.

Conceptually:

```cpp
struct NodeDefinitionEntry {
    NodeDefinitionId id;
    NodeDefinitionKind kind;
    DefinitionVersion version;
    ConfigureFunction configure;
    ConfigurationSignature signature;
    ConfigurationOperations argument_operations;
    std::vector<ModuleRef> module_refs;
};

struct NodeDefinitionsSnapshot {
    uint64_t generation;
    DefinitionMap by_id;
};
```

The exact field types can follow existing package/provider types. The important
contract is immutability and provider lifetime safety.

## One snapshot per batch

`NodeInstances` receives one `NodeDefinitionsSnapshot` for one complete
instantiation batch.

Every top-level request and every nested `g.node<"id">(...)` call in that batch
must resolve through that same snapshot. Nested resolution is an ordinary
lookup/callback inside `NodeInstances`' current configuration context, never a
new app-module event.

A new definitions generation arriving concurrently can only affect a later
batch. It cannot alter the definition world halfway through the current batch.

## Project configuration source

The initial generic project configuration surface stores a single C++
argument-list source string, not an app-defined typed JSON schema.

For example:

```cpp
FilterConfig{.cutoff = 1800.0f, .q = 0.7f}, Mode::stereo
```

The application does not split this string itself. Clang parses it in the
provider/callee translation-unit context where the demanded C++ types are
known.

A future UI may generate the same C++ argument-list source from dropdowns,
numeric fields, text fields, and other structured controls. That UI improvement
does not change the persistence/configuration ABI.

## Typed erased argument ownership

The existing erased configuration call boundary can remain, but cached
arguments must become owned and value-comparable.

For each registered configuration signature the provider/compiler should expose
typed operations generated in the translation unit that knows the real C++
types. Expected operations include:

- copy/clone;
- destruction;
- equality;
- hashing;
- optional move support where useful.

Do not infer semantic equality with raw bytes.

The expression compiler may materialize each argument as an LLVM global or
other retained typed object and pass its address through the existing erased
configuration interface. Any LLVM/code/data object backing retained pointers
must stay alive as long as the configured graph that can reference it.

## Two caches, two keys

There are two distinct reusable products:

1. expression compilation: `(definition version, C++ argument-list source)` ->
   typed owned argument tuple/thunk;
2. node instance configuration: `(definition version, typed argument values)` ->
   cached `NodeInstance`.

Nested module calls start at step 2 because their arguments are already typed
C++ values.

Value-sharing is an optimization. When a safe equality/hash operation cannot
be supplied for an argument tuple, `NodeInstances` may simply reconfigure that
invocation instead of sharing it.

## Meaning of `NodeInstance`

A cached `NodeInstance` is the immutable configured result of evaluating a node
definition with a particular configuration argument value set.

It is **not** one final runtime DSP object and is not one-to-one with a project
instance id.

Multiple external instance ids may resolve to the same cached node instance:

```text
project id A --\
               > NodeInstance cache entry X
project id B --/
```

Embedding X twice into the root builder creates two distinct graph placements.
Whole-graph lowering later allocates distinct runtime storage/state for those
placements.

A cache entry conceptually contains:

```text
owned typed argument tuple
ConfiguredGraph
stable local root/interface handle
provider/module lifetime references
definition-generation provenance
```

## Why the cache stores `ConfiguredGraph`

Do not retain a live `GraphBuilder`/`BuilderSession` as the reusable configured
instance representation merely to simplify embedding.

`BuilderSession` contains transient construction state beyond the semantic
configured graph, including definition/package context, recursion/cycle state,
pending allocations, temporary expression/ref state, and builder-local
mutation state. Sharing that state across embeddings would create unnecessary
coupling and hazards.

Instead, make frozen `ConfiguredGraph` embedding first-class. Both live-child
and frozen-child embedding should delegate to the same importer/remapper.

## Batched request/result contract

A project-owned request may look approximately like:

```cpp
struct NodeInstanceRequest {
    NodeInstanceId instance_id;
    NodeDefinitionId definition_id;
    std::string configuration_expression;
};
```

The actual API should be batched and revisioned. `ProjectGraph` owns the
canonical desired list and passes the complete requested set for one root-build
transaction; `NodeInstances` does not become a second owner of project intent.

One invocation of `NodeInstances`:

1. installs/uses exactly one definitions snapshot for that batch;
2. resolves/compiles requested project configuration expressions;
3. recursively evaluates all cache misses;
4. detects recursive definition/configuration cycles within that one context;
5. embeds all requested instances into the supplied root builder;
6. returns a complete instance-id -> embedding mapping;
7. returns batched diagnostics for unresolved/failed requests.

It must not publish one downstream app event per instance.

## Reload invalidation

Definition versions participate in cache identity. A provider reload therefore
cannot accidentally reuse a configured instance produced by an older provider
version unless compatibility/reuse is explicitly proven later.

On a new `NodeDefinitionsSnapshot`, `ProjectGraph` starts one new root-build
transaction and invokes `NodeInstances` once. `NodeInstances` can preserve cache
entries whose provider/version dependencies are unchanged and rebuild only
invalidated entries internally, but downstream modules observe one completed
batch result.

## Exact provider provenance survives into `GraphJit`

A cached `NodeInstance` must pin not only the configuration callback/data needed
to describe its frozen `ConfiguredGraph`, but also the exact primitive
implementation LLVM/provider revisions required to compile that configured graph
later.

When `ProjectGraph` embeds cached instances into the root graph, the resulting
configured generation must retain enough provenance for `GraphJit` to compile
exactly the same definition world. `GraphJit` must not query `NodeDefinitions`
for a newer snapshot after configuration has completed.

Initially `ProjectGraph` may pass the same immutable `NodeDefinitionsSnapshot`
alongside the root `ConfiguredGraph` if that is the simplest lifetime-safe
interface. The preferred long-term direction is for configured instances/root
graph provenance to pin only the exact provider LLVM/module references they
actually require.

See [graph_jit_direction.md](./graph_jit_direction.md).
