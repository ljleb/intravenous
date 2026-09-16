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

The retained construction surface now includes module/leaf configuration callbacks,
the registered signature callback, and provider-generated value operations for each
configuration parameter. `NodeDefinitionsSnapshot` also pins the exact accepted
`PackageRevision` objects whose provider/configuration tables define that generation,
so recursive configuration can create a complete `BuilderSession` without re-entering
the package pipeline.

The configuration-core `NodeInstances` checkpoint has also landed. `NodeInstances`
subscribes to the immutable snapshot event directly; it no longer consumes the
legacy package-definition diff. The old diff remains only as a temporary downstream
projection for module-source introspection.

The package-pipeline migration has landed. `NodeDefinitions` consumes only the
immutable accepted package-revision snapshot from `PackageDefinitions` and
derives the global node-definition namespace from it. Package declarations,
build candidates, accepted package revisions, and package-catalog state remain
outside this module.

## Implementation checkpoint: `NodeInstances` configuration core

The generalized app module is now named `NodeInstances`. Its compatibility
IV-module RPC/persistence surface remains temporarily, but definition realization
is driven by complete `NodeDefinitionsSnapshot` values and supports both leaf and
module definitions.

The first configuration-cache implementation now provides:

- provider-generated copy/destruction/equality/hash operations for typed erased
  configuration values;
- one-snapshot recursive definition resolution, including nested `g.node(...)`
  calls without another app-module event;
- owned invocation values whose lifetime is retained by the configured result;
- immutable `ConfiguredGraph` cache entries keyed by definition generation/version,
  tiled layout, and typed semantic argument values;
- deliberate reconfiguration when an owned argument type lacks safe equality/hash;
- transitive lifetime retention for nested configured results, including nested
  invocations that are not reusable cache entries;
- cycle detection and batched per-instance diagnostics;
- repeated placement of one cached configured result through the frozen-graph
  embedding API.

Cache invalidation is intentionally conservative at this checkpoint: publication
of a new definitions snapshot clears the reusable cache. Preserving entries whose
exact transitive provider dependencies did not change is a later optimization pass,
not a prerequisite for the application architecture.

The **C++ configuration-expression compiler has not landed yet**. The new
configuration core currently accepts already-typed erased arguments synchronously.
The next checkpoint is to compile the durable project argument-list source into an
owned typed thunk/tuple and feed that result into this same value-level cache path.

## Definitions are providers, not configured instances

`NodeDefinitions` owns one immutable versioned registry containing both leaf
node definitions and module node definitions. It is a **derived global namespace**,
not the owner of discovered packages, accepted package revisions, build queues,
or package catalog state; those package-level responsibilities belong to the
package pipeline described in
[package_pipeline_architecture.md](./package_pipeline_architecture.md).

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
    std::vector<std::shared_ptr<PackageRevision const>> package_revisions;
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

Each registered configuration signature now exposes typed operations generated
in the translation unit that knows the real C++ types. The current operations are:

- copy/clone;
- destruction;
- equality;
- hashing;
Move-only ownership remains a future extension; the first cache implementation
requires a safe provider-side copy operation for retained values.

Do not infer semantic equality with raw bytes. Equality/hash are optional for
reuse: a safely owned value without them is configured normally but is not shared
through the reusable cache.

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

Frozen `ConfiguredGraph` embedding is now first-class. Live-child and
frozen-child embedding delegate to the same importer/remapper, and each frozen
embedding returns an explicit local-to-parent translation without mutating the
cached local handles. Imported virtual-node records remain scope-distinct rather
than being merged into the parent root namespace.

## Batched request/result contract

A project-owned request may look approximately like:

```cpp
struct NodeInstanceRequest {
    NodeInstanceId instance_id;
    NodeDefinitionId definition_id;
    std::string configuration_expression;
};
```

The actual API should be batched and revisioned. `NodeInstances` owns the
canonical desired instance set. `ProjectGraph` carries an optional typed mutation
or replay batch into one `NodeInstances` invocation and receives the complete
realization/embedding result for the current root-build transaction; it does not
duplicate instance intent.

One invocation of the completed `NodeInstances` path will:

1. install/use exactly one definitions snapshot for that batch;
2. compile requested project configuration expressions into typed owned values;
3. recursively evaluate all cache misses;
4. detect recursive definition/configuration cycles within that one context;
5. embed all requested instances into the supplied root builder;
6. return a complete instance-id -> embedding mapping;
7. return batched diagnostics for unresolved/failed requests.

Steps 1 and 3-7 are implemented for already-typed arguments. Step 2 is the next
checkpoint and will sit in front of the existing value-level configuration/cache
primitive rather than changing it.

It must not publish one downstream app event per instance.

## Reload invalidation

Definition versions participate in cache identity. A provider reload therefore
cannot accidentally reuse a configured instance produced by an older provider
version unless compatibility/reuse is explicitly proven later.

On a new `NodeDefinitionsSnapshot`, `ProjectGraph` will start one new root-build
transaction and invoke `NodeInstances` once. The current implementation clears its
reusable cache on snapshot publication for correctness. A later optimization pass
may preserve entries whose exact provider/version dependencies are unchanged;
downstream modules must still observe only one completed batch result.

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
