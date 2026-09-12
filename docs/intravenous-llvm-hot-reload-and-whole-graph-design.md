# Intravenous: LLVM Hot Reload, Registered Nodes, and Whole-Graph Execution

_Status: consolidated architecture direction based on the current `feature/llvm-module-reload` branch and the subsequent design discussion. This is intentionally more concrete than a direction note, but it is not a line-by-line implementation plan. Items marked **decided** are intended constraints; items marked **provisional** are design choices that should be validated by implementation and profiling._

## 1. Purpose

This document consolidates two related pieces of work:

1. the LLVM/Clang-based C++ hot-reload pipeline that replaces the old constexpr/reflection module build; and
2. the next execution architecture, in which registered node implementations, cached `ConfiguredGraph`s, project-wide connections, and whole-graph LLVM lowering converge into a fast reloadable project kernel.

The goal is not merely to move existing runtime machinery into LLVM. The goal is to preserve the concise C++ configuration model while removing execution costs that only exist because the current generic runtime has to discover graph structure dynamically.

The desired end state is roughly:

```text
registered node implementations
        +
cached ConfiguredGraphs
        +
project module instances
        +
project connections
        |
        v
whole-project graph lowering
        |
        +-- resolve iv modules into their managed graphs
        +-- resolve virtual-node/member ports
        +-- gather complete producer/consumer relationships
        +-- lower connection semantics
        +-- detect SCCs / feedback
        +-- choose execution regions and block quanta
        +-- analyze history / latency / lifetime
        +-- choose storage and reuse
        +-- lower TTL / activity
        |
        v
generated LLVM graph kernel
        +
registered node LLVM implementations
        |
        v
graph-specific optimization
        |
        v
native realtime kernel
```

The existing lane/DSP graph convergence remains an important later direction, but it is not part of the first whole-graph execution rewrite. The new kernel compiler should be placed below the current graph source so that the future canonical project graph can replace that source without rewriting the kernel compiler.

---

## 2. Vocabulary

The codebase should return to a small vocabulary. New near-synonyms should not be introduced merely because different compiler stages manipulate the same concept.

The preferred words are:

- **graph**: a set of nodes and connections;
- **node**: something that appears as one node to graph configuration and project wiring;
- **node type**: a primitive C++ implementation registered independently with the server;
- **iv module**: a registered graph-producing definition that owns/manages a subgraph and can itself be instantiated as one node;
- **IV package**: an independently discovered, watched, built source package that may provide any number of node types and iv modules;
- **module instance**: an instance of a registered iv module in another graph or in the project;
- **virtual node**: the existing stable source-level identity used to address a node and, where applicable, its direct members;
- **subgraph**: local structure created inside one `GraphBuilder`; it does not imply an optimizer boundary;
- **port**: the public graph connection point; do not call it an endpoint, surface, socket, etc.;
- **channel**: a member of a sample port layout;
- **connection**: a graph relationship from an output port to an input port;
- **wiring**: generated execution code implementing one or more connections; use this for implementation, not as a second persistent graph noun;
- **state**: mutable runtime node state that survives kernel replacement when compatible;
- **lowering**: a compiler transformation from a richer graph representation to a more execution-specific one;
- **region**: a scheduling unit when the whole graph must be processed with a particular execution quantum;
- **execution plan**: the finalizer's derived schedule/state/storage description before or alongside LLVM generation;
- **finalizer**: the build/compiler stage that has the complete graph and emits the finalized native artifact/kernel;
- **cache**: persisted reusable results whose invalidation is explicit.

A useful invariant is:

> If two structures both represent graph ports, they should normally still be called ports. Stage qualifiers belong in type names only where they materially distinguish representation, not in the conceptual vocabulary.

---

## 3. Current branch: what already exists

The current branch has already completed the fundamental hot-reload migration from GCC/constexpr reflection to Clang/LLVM configuration.

### 3.1 Current `ConfiguredGraph`

The current repository explicitly describes `ConfiguredGraph` as the lossless configuration representation:

```cpp
struct ConfiguredGraph {
    GraphBuilderIdentity identity{};
    GraphBuilderNodeBundles node_bundles{};
    GraphBuilderConnections connections{};
    GraphBuilderPublicPorts public_ports{0};
    GraphBuilderDetach detach{};
    GraphBuilderAnnotations annotations{};
    GraphBuilderVirtualNodes virtual_nodes{};
};
```

This is an important architectural property. Once a `GraphBuilder` is finished, lowering should not need to reach back into mutable builder state.

### 3.2 Current builder boundary

`GraphBuilder` is already a facade over an opaque `BuilderSession` owned by the shared builder library. Mutable containers and graph bookkeeping have been moved out of the module-facing C++ template layer.

Today, however, node construction remains type-based:

```cpp
g.node<Gain>(0.25f);
```

and that type-based call still creates a type-specific `NodeBuildRequest`, compiler record, and node description inside the consuming C++ build. The new registration design described later intentionally changes this.

### 3.3 Current compiler records

The current module/finalizer bridge uses a build-local `NodeCompilerRecord`:

```cpp
struct NodeCompilerOperations {
    std::size_t (*declare_node)(...);
    void (*tick_block)(...);
    void (*skip_block)(...);
};

struct NodeCompilerRecord {
    NodeCodeKey code_key{};
    NodeCompilerOperations operations{};
    char const* type_name = nullptr;
    std::size_t type_name_size = 0;
    std::size_t state_size = 0;
    std::size_t state_alignment = 1;
};
```

`NodeCodeKey` is deliberately build-local. It is a compiler join between an configured node instance and LLVM functions in that build, not a persistent server identity.

The current source plugin discovers node types through emitted `node_compiler_record<T>` specializations. This is a precise signal for the current migration, but it is not the desired long-term ownership model because using the same C++ node type in many IV packages can cause its compiler-facing implementation to be emitted repeatedly.

### 3.4 Current Clang source plugin

The Clang plugin replaces old `std::meta`/GCC-plugin responsibilities. It already performs source introspection and type/state discovery and can associate C++ declarations with Clang identities.

The design continues to use:

- Clang AST information for source identity and source annotations;
- Clang USRs or equivalent compiler identities for build-time declaration/type joins;
- AST record layout for node `State` metadata;
- explicit injected source-recording calls rather than `std::source_location`.

Clang identities are compiler plumbing. They are not intended to become persistent project IDs.

### 3.5 Current configuration relocation

The current branch already supports node configuration pointers into retained LLVM globals.

During configuration JIT execution, a pointer field may refer to memory materialized for a retained LLVM global. The finalizer maps that JIT address back to the corresponding master-module global plus an addend and emits a real LLVM constant relocation rather than serializing a meaningless process-local address.

This allows configurations such as:

```cpp
struct Node {
    float const* table;
};
```

where `table` can refer to a retained constant array/string/table defined in the C++ build.

This capability is part of the new design. Registered-node public configuration must not be constrained to pointer-free POD data merely for convenience.

### 3.6 Pre-registry correctness/performance checkpoint

Before the IV-source/registry refactor, the design starting point recorded:

```text
100% tests passed, 0 tests failed out of 443

heavy: 44 tests
light: 399 tests
real test time: ~20.54 s
```

Representative hot module reload profiles are approximately:

| Module | Hot pipeline | Export | Link | Finalizer | JIT | Runtime O3 | Object emit | Native link |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| brown_duda | 1264 ms | 862 | 370 | 361.7 | 110.2 | 80.5 | 105.5 | 36.3 |
| dc_offset | 594 ms | 457 | 105 | 97.6 | 17.8 | 15.3 | 21.4 | 37.5 |
| q24_bspline_pan | 1053 ms | 684 | 341 | 333.0 | 86.3 | 81.3 | 105.7 | 38.7 |
| q24_icosphere_pan | 1541 ms | 874 | 634 | 622.2 | 142.4 | 178.2 | 212.5 | 36.7 |
| q24_pan | 1051 ms | 672 | 350 | 340.3 | 84.3 | 85.1 | 111.7 | 38.4 |
| saw | 945 ms | 615 | 299 | 288.6 | 82.5 | 67.6 | 77.6 | 40.3 |

The existing profiler demonstrated that measurement and compiler-stage visibility should be treated as architecture, not optional debugging polish. The whole-graph compiler should be developed with the same discipline.

---

## 4. Current LLVM hot-reload pipeline

This section records the hot-reload pipeline that is being preserved as the
foundation for the next work.  The historical per-source configuration step is
shown first because it explains the original finalizer split; the current
source/registry implementation changes where cross-source configuration happens.

### 4.1 High-level pipeline

```text
source edit
    |
    v
DependencyWatcher / IvModuleReload
    |
    v
discover IV package packages
    |
    v
existing custom CMake OR default CMake
    |
    v
Clang frontend once
    |
    v
LLVM bitcode / master LLVM module
    |
    v
iv_module_finalize
    |
    +----------------------------------+
    |                                  |
    v                                  v
clone configuration-reachable IR      retain master LLVM
    |                                  |
    v                                  |
source configuration entrypoints           node code
    |                                  |
    v                                  |
serialize node/config/compiler data
preserve source configuration IR
optimize retained runtime/native IR
emit native object
invoke original CMake link command
                   |
                   v
signature-addressed source artifact
                   |
                   v
load every discovered source artifact into the shared configuration generation
                   |
                   v
invoke the requested registered IV_MODULE builders
                   |
                   v
fully realized ConfiguredGraph
                   |
                   v
current GraphLowerer / GraphCompiler compatibility path
                   |
                   v
current runtime graph
```

### 4.2 One C++ frontend pass

A central property of the migration is that changed module C++ is parsed by Clang once.

The old pipeline required constexpr graph configuration and generated-source/reflection work. The new pipeline preserves the parsed LLVM module and executes only an configuration clone through ORC.

The configuration JIT is therefore a consumer of the same frontend result that later supplies node implementation LLVM.

### 4.3 Finalizer configuration clone

Historically, the finalizer:

1. reads LLVM input from the intercepted link;
2. scans emitted node compiler records;
3. retains a master module;
4. builds an configuration clone containing the functions/globals reachable from the graph-building entry point;
5. adds a JIT-global-address table so executed pointer values can later be related back to retained LLVM globals;
6. loads native dependencies needed by configuration;
7. runs global initializers;
8. executed each registered `IV_MODULE` builder against its own
   `BuilderSession`/`GraphBuilder`;
9. captured and serialized the resulting `ConfiguredGraph`;
10. resolved relocatable configuration pointers;
11. removed the configuration JIT generation; and
12. pruned configuration-only IR from the retained module.

The source/registry implementation deliberately stops at the independently
compiled source artifact.  It retains exported source registration and
configuration entrypoints.  The host loads the artifacts for the participating IV
sources, then invokes registered iv-module builders only after that complete
configuration generation is available.  This is what permits `g.node<"id">()` to
resolve another source without merging that source's C++ files into the
consumer target.

This separation should remain conceptually intact even as registered node types and multiple iv modules per IV package are introduced.

### 4.4 CMake remains authoritative

The system must preserve both:

- the default IV module CMake project; and
- user-provided/custom CMake environments.

`iv_add_package(target)` exposes `target` as an ordinary CMake **OBJECT**
library, while `${target}__finalized` produces the `.ivpkg.bc` artifact.  This
keeps the package's own CMake authoritative: custom projects may use normal
`target_compile_options`, `target_include_directories`,
`target_compile_definitions`, and `target_link_libraries` calls without
creating a package DSO.
`${target}__finalized` exists as soon as `iv_add_package()` returns, so custom
CMake may add its own dependency edges before IV collects the completed link
requirements at the end of that directory.

Those link requirements intentionally have three distinct meanings:

- an OBJECT library or full-LTO STATIC library is an **LLVM input**. Its
  objects/archive members are linked into the package bitcode by the finalizer;
- a SHARED, MODULE, or imported native-library target is a **dynamic native
  dependency**. CMake emits its resolved library path beside the artifact, and
  the loader installs it as a symbol generator in that package revision's ORC
  `JITDylib`;
- an INTERFACE target or bare CMake link item is a **header/usage
  requirement**. It keeps its ordinary CMake compile semantics and does not
  imply that native code is copied or loaded.

For ambiguous external paths, package CMake must say which meaning it intends
with `iv_package_add_llvm_inputs(target ...)` or
`iv_package_add_dynamic_libraries(target ...)`.  A native archive with no LLVM
bitcode members is an error, rather than silently being treated as either kind
of dependency.  This makes native ownership explicit and avoids quietly
recreating a per-package operating-system DSO boundary.

This preserves custom:

- compile definitions;
- library search paths;
- native-library declarations;
- JUCE dependencies;
- platform flags;
- arbitrary user CMake configuration.

The future registry/source design must not regress this property.

### 4.5 Generation loading remains useful

The current loader builds a signature-addressed `.ivpkg.bc` source artifact,
adds it to one shared `LLJIT` in a new package-revision `JITDylib`, validates
its exported ABI/data, and holds its resource tracker alive for every
realization that uses it. A changed source therefore receives a new ORC
revision while graphs that still use an earlier source generation retain the
old code and its dynamic-library generators.

`iv_builder` is the one project runtime DSO. Package artifacts are LLVM in the
shared ORC, not operating-system DSOs. Their `JITDylib`s resolve `iv_builder`
explicitly, then package-declared dynamic native libraries, then ordinary
current-process symbols.

Even if the eventual realtime executor becomes a generated project kernel, the general generation model remains useful for:

- atomic publication;
- retaining old code/state until the replacement is valid;
- failure isolation;
- state migration;
- avoiding mutation of a live generation in place.

### 4.6 Current final native optimization

The current finalizer runs a default LLVM optimization pipeline, including O3 for runtime mode, then emits a native object and resumes the original link.

The future whole-project finalizer should not assume that blindly running the full default O3 pipeline over all retained code is always the fastest reload path. Node implementations can be cached/preoptimized independently, and graph-specific optimization can later be narrowed to the reachable kernel closure. This is a profiling decision, not an up-front requirement.

---

## 5. Architectural direction: independently registered graph units

### 5.0 Branch boundary

The implementation begins with the **IV-source/registry representation** in
this branch. The first source-only change makes `iv_package.json` the sole
source-package manifest; it contains build discovery data only, never a
module identity or entry function. `IV_MODULE`/`IV_NODE` registrations are the
source of stable definition identities, and one source reload publishes its
complete definition set (including zero or many iv modules).

The generic dynamic API needs no generated provider header at all:

```cpp
auto node = g.node<"some.registered.id">();
```

The stable string is sufficient for C++ compilation.  A source remains the
unit watched, compiled, and transactionally replaced; `GraphInputLanes` and
module instances consume published iv-module definitions, never an IV package.
Compiler-produced registration data, not a scan of source text or generated
includes, is authoritative for associating an ID with a source artifact.

The source-list protocol therefore reports one source package with its
`sourceId`/root plus the currently published `moduleIds` and `nodeTypeIds`.
It does not manufacture one source-list item per macro spelling. A node-only
source is visible with an empty `moduleIds` list and its registered node-type
IDs; a source with several `IV_MODULE`s exposes every instantiable module ID.

### 5.0.1 Package lifecycle is not definition publication

The package browser must distinguish these control-plane facts rather than
turning every empty `moduleIds` list into “no module exists”:

```text
discovered package
    -> queued / building
    -> built artifact
    -> validated, published definition set

or

discovered package
    -> queued / building
    -> failed (diagnostic retained; prior valid definitions remain live)
```

`IV_MODULE(...)` is compiler-produced registration data. Before a package has
successfully compiled and been loaded, the server cannot honestly claim a
module ID is published; it must instead report that the package is queued,
building, or failed and retain the diagnostic. A successfully built package
with no module IDs is then an intentional empty/node-only package, not an
ambiguous failure state.

Server readiness is independent from package discovery and compilation.
Persisted module instances retain their package declarations themselves;
filesystem discovery is a second declaration source, not the owner of those
references. Startup can therefore start RPC first, then the package control
service scans, builds, and publishes candidates. This keeps the UI responsive
and, crucially, makes first-start build errors observable by the connected
client. Publishing does not wait for an audio task pass: package registry state
must progress even when no project graph is executing.

The persisted `ivModuleInstances.create` record therefore includes a
project-relative `package_root` alongside the stable module ID. Replay creates
the desired instance and retains that package declaration even if its
definition has not yet been published; publication realizes the same instance
later. A project file must not rely on startup discovery racing quickly enough
to resolve a bare module ID.

The browser queries the declaration registry and the published registry as
snapshots. It must not rescan the filesystem and reconstruct ownership from
incremental definition-change notifications, because those are two different
states and can briefly disagree during discovery, build, conflict, or reload.

The compatibility-runtime API is deliberately **dynamic**: every
`g.node<Id>(...)` returns `NodeRef`. It does not generate a C++
`node_interface<Id>`, try to infer a static port shape, or make a consumer's
source build depend on a provider-facing header. A registered primitive node
is a zero-argument leaf; an implementation that needs caller-supplied graph
configuration is exposed as an `IV_MODULE` wrapper instead.

Package invalidation is deliberately conservative in this stage. A watched
package edit queues the declared package set for reload and reconfigures any
consumer that realizes the changed definition. There is no intended
interface-hash-versus-definition-hash optimization boundary here: a provider
change means its consumers reload. This keeps the dynamic API simple and
avoids treating static typing as an architectural prerequisite.

This branch establishes registration identities, source-local configuration
outputs, conservative reload, and the existing per-module compatibility
runtime. The current finalizer continues to produce and load that runtime
graph; it is not reshaped around the later whole-project execution design.

The whole-project finalizer and the generated execution model (sections 12
through 25) are deliberately deferred to a separate branch. They must consume
the source/registry representation published here, rather than shape it around
the current per-module runtime.

### 5.1 Decided: callers create nodes by stable ID

The preferred C++ configuration operation becomes:

```cpp
auto filter = g.node<"audio.filter">(...);
```

The caller always receives a graph node, regardless of how the ID is implemented.

A stable ID may resolve to:

- a primitive registered **node type**; or
- a registered **iv module** whose implementation owns/manages a subgraph.

The call site deliberately does not know which.

This gives a valuable replacement property:

```text
"audio.filter" implemented by one C++ node type
        |
        | later
        v
"audio.filter" implemented by an iv module containing many nodes
```

If the realized dynamic ports remain usable by callers, no caller source needs
to know which implementation kind supplied them.

### 5.2 Registration spellings

The intended direction is conceptually:

```cpp
struct Gain {
    // ...
};

IV_NODE("iv.gain", Gain);


void voice(GraphBuilder& g, /* optional extra args */)
{
    auto gain = g.node<"iv.gain">(...);
    // ...
}

IV_MODULE("iv.voice", voice);
```

The exact macro expansion remains an implementation detail. The important semantics are:

- `IV_NODE(id, T)` registers a primitive node implementation under `id`;
- `IV_MODULE(id, fn)` registers a graph-producing function under `id`;
- both expose the same dynamic `NodeRef` construction result to callers;
- both participate in one stable-ID registry;
- a C++ call site uses only `g.node<id>(...)`.

### 5.3 One ID namespace

**Provisional but preferred:** node types and iv modules should share one ID namespace.

An ID identifies one registered graph node definition. Its implementation kind is private.

Therefore these should collide:

```text
IV_NODE("iv.filter", Filter)
IV_MODULE("iv.filter", filter_graph)
```

The registry should not permit two active providers of the same ID, even if their implementations happen to be compatible or identical.

### 5.4 IV package

An **IV package** is the loader/build-level unit.

An IV package may provide:

- zero or more node types;
- zero or more iv modules;
- only node types;
- only iv modules;
- an experimental node type immediately beside an iv module using it;
- multiple C++ translation units under custom CMake.

This is intentionally not called an iv module. An iv module is a graph definition; an IV package is the package the loader watches/builds.

`iv_package.json` is the IV-source manifest. Its role is build/package discovery, not a redundant registry of identities already registered in C++.

Every compilation belonging to a source package carries that package's
canonical source-root identity. Registration ownership must use this explicit
package identity, not a compiler-dependent spelling of `__FILE__`; custom
CMake sibling translation units are therefore providers of the same source
package while their declaration locations remain available as metadata.

The manifest should describe how to build the source, not redundantly enumerate identities already registered in C++ unless a real use case requires that duplication.

---

## 6. Server registry

### 6.1 Node type registry

Node types must be registered independently of C++ iv modules.

Conceptually:

```text
NodeTypeId
    -> NodeTypeDefinition
```

A node type definition contains what the server/finalizer needs to instantiate, inspect, compile, and migrate the primitive node, for example:

```text
stable id
state layout / state metadata
lifecycle implementation
LLVM tick/skip/declare implementation
source/type metadata
configuration construction entry
```

This is the mechanism that prevents the same primitive implementation from being copied into every iv module that uses it.

If 100 iv modules use `iv.gain`, the desired system contains one registered `iv.gain` implementation artifact, not 100 copies of `Gain::tick` and lifecycle wrappers.

### 6.2 IV module registry

Registered iv modules are also keyed by stable ID:

```text
IvModuleId
    -> cached ConfiguredGraph
       dependency IDs
       source metadata
       compiled configuration entry
```

The internal implementation remains a `GraphBuilder` function, but consumers
only name the ID and receive the dynamically realized result.

### 6.3 Registry updates are transactional per IV package

Each IV package build should publish a complete candidate set of definitions:

```text
SourceDefinitions {
    nodes;
    iv_modules;
}
```

The server atomically replaces that source's previous **candidate** set. It
then validates the complete candidate registry before publishing a new valid
generation. Validation failure publishes no partial per-ID result: every live
map, ownership record, and consumer notification continues to describe the
previous complete generation.

Registry validation includes:

- duplicate stable IDs across IV packages;
- duplicate IDs within one IV package.

Missing `g.node<Id>` providers and iv-module dependency cycles are rejected
while executing the shared configuration generation for the affected package.
The failed package keeps its previous candidate/live generation; the registry
does not publish a partial result.

The live project/kernel should remain on the last valid generation while the candidate registry is invalid.

### 6.4 Moving definitions between IV packages

This transactional model deliberately supports cut/paste moves where files are saved in either order.

If the destination is saved first:

```text
source A provides "iv.foo"
source B provides "iv.foo"
        -> temporary collision
```

The candidate registry is invalid, but the live generation remains valid. Once A is saved without the definition, B becomes the unique provider and the registry can publish.

If the source is saved first:

```text
source A no longer provides "iv.foo"
        -> successful empty candidate for A
        -> "iv.foo" is unpublished
```

This is a valid new registry revision. Any desired project instance of
`"iv.foo"` remains visible as unrealized so the user can delete it; desired
project state is not a registry-validation input. When the destination is
saved, the ID publishes from B again.

No special “move detection” is required, and stable IDs never depend on source paths.

---

## 7. Dynamic registered-ID configuration

`g.node<Id>(...)` is intrinsically available for every stable ID. It needs no
generated provider header, source-to-ID scan, or textual inclusion of provider
implementation code merely to name `Id`.

Every registered-ID call returns `NodeRef`. The actual public ports are the
ports of the immediately realized node or embedded iv-module graph, and are
therefore available to the existing dynamic/indexed DSL after configuration.
The public API deliberately has no `node_interface<Id>` specialization,
typed-return recovery, or provider-generated include protocol.

This is a semantic simplification, not a bootstrap compromise. Static typing
would require a second public contract whose correctness and invalidation rules
are separate from the executable configuration result. The current system has
one authority instead: compiler-produced package definitions and the
`ConfiguredGraph` they realize.

### 7.1 Package dependencies are executable dependencies

An IV package using another stable ID does not compile that provider's C++
implementation into its own artifact. It resolves the provider from the shared
configuration generation and executes its already-compiled configuration entry
immediately.

The reload policy is intentionally broad:

```text
provider package changes
    -> reload declared IV packages
    -> reconfigure every consuming package definition
```

There is no static-interface fast path to preserve. This is correct for both
primitive nodes and iv modules, and makes replacement of one implementation
kind with the other private to the provider so long as the realized graph
remains usable by its consumers.

### 7.2 Same-package freshness

A registration in the current translation unit is authoritative for later
`g.node<"id">()` calls in that translation unit during the same compile. The
generic dynamic path avoids stale generated metadata and explicit-specialization
redefinition problems entirely.

### 7.3 Configuration arguments

The registered primitive-node form is intentionally a zero-argument leaf:

```cpp
IV_NODE("iv.gain", Gain);
```

An implementation requiring caller-supplied graph configuration exposes an
`IV_MODULE` function and forwards those values through its local
`g.node<"id">(...)` calls. The `IV_MODULE` adapter checks the function's
concrete argument count and types when it executes. This avoids inventing a
second, provider-independent constructor schema for arbitrary C++ node types.

The project-level module-instance surface remains zero-argument in this
compatibility runtime. Persistent construction configuration, if it becomes a
product requirement, belongs with project-instance persistence and the later
project connection/finalizer representation rather than in generated C++
headers.

---

## 8. Configuration values at package boundaries

Cross-package construction does not reproduce arbitrary primitive-node C++
constructors in a consumer. The dynamic ID API instead calls a provider's
already-compiled `IV_MODULE` configuration entry, whose actual function
signature is checked at invocation time.

Existing relocatable pointer configuration remains supported. A pointer into
retained LLVM global data is represented through the existing relocation path,
not treated as a raw cross-package process address. The configured graph retains
the package revisions it used, so caller-owned globals and provider-owned code
remain alive together.

This branch intentionally has no serialized, UI-authored generic constructor
schema, overload/default metadata, or project-instance argument persistence.
Those are separate product/API decisions, not requirements for dynamic
registered-ID configuration or package reload.

---

## 10. Registered ID configuration dependencies

### 10.1 `g.node<Id>` greedily realizes a dependency

When an iv module contains:

```cpp
auto x = g.node<"iv.gain">(...);
```

its configuration execution resolves the stable ID `iv.gain` through the current
configuration generation. The provider implementation source is not included in
the caller, but its already-compiled configuration entry is executed immediately.

For a primitive node this produces an ordinary concrete/tiled node bundle.
For an iv module it executes the child builder and embeds an ordinary
`SubgraphNodeBundle`. A registered ID is therefore an configuration operation,
not a persistent configured-graph node kind.

### 10.2 No unresolved registered-node boundary

`g.node<"some.module">()` must not fabricate a placeholder boundary or an
invented port interface. The returned `NodeRef` is the actual realized child,
so named/indexed sample ports, event ports, layouts, zero outputs, and dynamic
output counts all retain their existing DSL meaning.

A finished `ConfiguredGraph` is consequently self-contained and fully realized:
there are no deferred graph-production operations for downstream lowering to
interpret.

### 10.3 Node types are leaves; iv modules recurse during configuration

For dependency validation and finalization:

```text
registered node type
    -> primitive leaf

registered iv module
    -> compiled configuration entry
       -> executes and embeds child graph
```

This gives a simple module-dependency graph.

### 10.4 Dependency cycles are invalid

Iv-module dependency cycles should fail candidate registry publication.

Example:

```text
A -> B
B -> C
C -> A
```

The server reports the cycle and keeps the last valid registry/project generation active.

The configuration generation also keeps an execution stack. That stack is the
authoritative check for argument/control-flow-dependent realizations and
reports the concrete `A -> B -> C -> A` request chain before recursion occurs.

### 10.5 Conservative provider invalidation

This branch has one definition revision, not separate public-interface and
implementation hashes. The dependency watcher queues the declared IV package
set when a package artifact changes, which reloads and reconfigures consumers
of the changed registered ID.

That policy is intentionally stronger than the minimum necessary. It gives a
correct and understandable result for every dynamic `NodeRef` shape without
inventing generated C++ interfaces or an interface-hash dependency graph.
More selective rebuilds are a future performance optimization only if profiling
shows that the broad package reload is material.

---

## 11. What is cached

### 11.1 Retain compiled configuration code and the current realization

Do not cache `BuilderSession`.

`BuilderSession` is mutable configuration machinery. It contains the data structures used to construct a graph efficiently, not the durable semantic result.

The current artifact boundary is:

```text
Clang/finalization per changed IV package
    -> reusable source configuration artifact
    -> current configuration generation

An `ConfiguredGraph` is the completed result of the zero-argument registered
iv-module realization. It is retained with the currently published definition;
there is no separate persistent realization cache or invocation memo table in
this compatibility runtime.
```

The current compatibility implementation retains configuration entrypoints in
the signature-addressed `.ivpkg.bc` artifact and invokes them from the shared
ORC after all participating package revisions are loaded. Each loaded revision
has a resource tracker and remains alive through configured-graph `ModuleRef`
ownership, so old and new revisions can coexist without pointer rewriting.
The remaining disk boundary is the finalized bitcode cache; replacing Clang
compilation/finalization with a persistent in-memory service is separate work.

If downstream work later discovers it needs information currently present only in `BuilderSession`, that information should be added to the retained configured result rather than making the mutable session persistent.

### 11.2 `ConfiguredGraph` remains lossless

The retained `ConfiguredGraph` should not be a reduced execution graph.

It must retain everything necessary to:

- instantiate the iv module later;
- address its virtual nodes and direct members;
- connect to their ports;
- preserve tiling and aggregate-node semantics;
- preserve subgraph information;
- preserve public ports;
- preserve configured local connections;
- preserve detach/feedback declarations;
- preserve TTL and other execution declarations;
- preserve source/provenance/annotation information;
- reconcile reloads;
- inspect the module without rerunning configuration;
- eventually lower the graph after all project connections are known.

The representation may be rephrased/canonicalized later if profiling or implementation convenience proves useful. For now it is immutable state attached to the live definition, not a new disk-cache format.

### 11.3 Node implementation code is not owned by an `ConfiguredGraph`

An `ConfiguredGraph` contains ordinary realized primitive node descriptions, but
does not own or duplicate their implementation LLVM. Primitive code belongs
to the node-type registry.

This changes the ownership model from today's per-TU `NodeCompilerRecord<T>` emission.

### 11.4 IV package compiler artifact may still retain globals

Although node implementation LLVM is registered independently, an IV package may still need retained LLVM globals because configured arguments/configurations can symbolically point into them.

The package artifact retains source-owned globals or other configuration data
that must survive while its configured graphs are live.

The exact packaging can be optimized later; the dependency must not be lost.

### 11.5 Do not persist every compiler stage preemptively

The presence of an intermediate representation does not automatically justify another disk cache.

Initially persist only the finalized package bitcode artifact that has a clear
build-time value. Registered node types and `ConfiguredGraph`s are retained in
the live package registry, not separately serialized.

SCCs, schedules, storage plans, generated LLVM, and other whole-project derived data should be recomputed until profiling demonstrates a need to cache them.

---

## 12. Project module instances and project connections

### 12.1 Definition versus instance

A published registered iv module has one retained, zero-argument
`ConfiguredGraph` realization in the compatibility runtime.

A project may contain many instances of that definition.

The instance supplies project identity and runtime state; the definition supplies the cached configured graph.

Conceptually:

```text
registered iv module "voice"
    -> cached ConfiguredGraph realization(s)

project:
    voice instance A
    voice instance B
    voice instance C
```

The definition is not copied merely because it is instantiated multiple times.

### 12.2 Cross-module connections do not use concrete node IDs

A project connection into an iv module cannot persist an `ConfiguredGraph` concrete node ID. Concrete IDs/handles are generation-local implementation details.

The stable addressing scheme is based on the existing virtual-node model:

```text
module instance
    /
virtual node identity
    /
virtual node itself OR direct member index
    /
port
    /
channel where needed
```

This allows a project to connect to:

- the aggregate port of a tiled stereo node;
- an individual direct member of that virtual node;
- a normal virtual node port;
- the stable port exposed by an iv module instance.

If the corresponding virtual node/member disappears after reload, the project connection becomes dangling. It is not silently retargeted to a coincidental concrete node.

### 12.3 Tiled nodes remain addressable as nodes

A tiled node is not flattened away before project connections are resolved.

For example, a stereo tiled node may have one virtual-node identity and an aggregate stereo port, while also containing two ordered direct members.

The project can connect to the tiled node as one node without exposing or depending on its internal concrete members.

Execution lowering may later expand the tile into concrete primitive operations. That is an execution transformation, not an configuration/project identity transformation.

### 12.4 Project connection object

The project needs a small persistent object describing cross-instance connections.

The exact C++ type names can follow current project-state conventions, but conceptually a connection stores:

```text
source:
    module instance
    virtual node id
    aggregate/direct-member selection
    output port
    channel if relevant

target:
    module instance
    virtual node id
    aggregate/direct-member selection
    input port
    channel if relevant
```

Use existing `port` terminology. Do not invent a second graph concept merely because the connection is cross-module.

### 12.5 Project connections never mutate cached definitions

Connecting project node A to a port in module instance B changes project state, not B's cached `ConfiguredGraph`.

The cached definition merely says that the addressed virtual node/member/port exists and how it maps to its configured graph.

This makes definitions immutable/shareable and prevents user project state from contaminating source-definition caches.

---

## 13. Whole-project finalization

The finalizer becomes the first stage allowed to make decisions that depend on all active module instances and all project connections.

### 13.1 Finalizer inputs

Conceptually:

```text
active module instances
    -> cached ConfiguredGraphs

project connections

node type registry
    -> state/lifecycle metadata
    -> LLVM implementation

source-owned retained LLVM globals as required

kernel specialization:
    block size B
    sample rate
    target triple / CPU / features
```

### 13.2 Do not physically merge builders first

Do not recreate one huge `BuilderSession` merely to call existing builder mutation functions.

Do not require physical concatenation into one giant `ConfiguredGraph` before lowering.

The finalizer can keep each cached graph locally numbered and introduce temporary project-wide compiler IDs while resolving instances.

If existing `GraphBuilder::subgraph()`/embedder logic contains useful semantic rules, extract/reuse those rules. Do not make mutable builder state the project-composition model.

### 13.3 Registered iv modules are already resolved

When a registered node ID denotes an iv module, its child graph was executed
and embedded during configuration.  Whole-project finalization therefore receives
the completed invocation graph rather than a recursive registered-ID request.

It composes the top-level project module instances and their project-owned
connections, then lowers ordinary virtual/tiled/subgraph structure as needed.

Module identity may remain attached for state/provenance/debugging, but it must not become an execution optimization boundary.

### 13.4 Resolve virtual nodes and direct members before flattening

Project connections are first resolved against the current iv-module instance's virtual-node structure.

Only after all cross-module connections have been resolved is it valid to flatten tiling/virtual/subgraph implementation structure for execution.

This ensures that project-owned connections can target aggregate/tiled/virtual ports without exposing their internal concrete representation.

### 13.5 Complete producer/consumer knowledge first

All local `ConfiguredGraph` connections and all resolved project connections are combined before connection implementation is chosen.

At this point, and only at this point, the compiler knows:

- every producer of each input;
- every consumer of each output;
- global fanout;
- channel/layout conversions required across all uses;
- cycles introduced across module boundaries;
- which values are observable by project outputs or side effects.

These facts drive the execution model.

---

## 14. `ConnectionNode` and connection lowering

### 14.1 `ConnectionNode` is transitional execution machinery

The existing graph uses generated `ConnectionNode` machinery to implement fan-in, conversion, defaults, runtime contributions, and copying.

The new whole-graph compiler should preserve the semantics but should not assume a `ConnectionNode` object must exist at runtime.

### 14.2 Lower connections into expressions/wiring

Once complete producers are known, an input can be represented conceptually as:

```text
B.input =
    convert(A.output)
  + convert(C.output)
  + default contribution
  + runtime/project contribution
```

The finalizer then chooses how this is implemented in generated LLVM.

Possible outcomes include:

- direct SSA use;
- inline conversion;
- one shared conversion reused by several consumers;
- fused sum/mix arithmetic;
- reusable block scratch;
- persistent storage where temporal semantics require it.

No gather/copy/convert temporary buffer is implied by the graph semantics themselves.

### 14.3 Migration seam for later lane deletion

For the first runtime project, the current lowerer/`ConnectionNodeSpec` can be translated into the new connection semantics.

Later, after lane/DSP graph convergence, the canonical project graph can feed the same connection-lowering stage directly.

Therefore the new execution compiler does not depend on deleting the lane graph first.

---

## 15. Block size and kernel specialization

### 15.1 Decided: host block size is known during kernel generation

The host/audio execution block size `B` should be a kernel-specialization parameter.

It should **not** be a parameter of C++ node source compilation or registered node identity.

Pipeline:

```text
cached node LLVM + cached ConfiguredGraphs + project graph
        |
        v
KernelSpecialization {
    block_size = B,
    sample_rate,
    target CPU/features
}
        |
        v
schedule/storage/LLVM generation
```

Changing `B` recompiles the project kernel, not the node implementations or iv-module C++ sources.

### 15.2 Why fixed `B` matters

A fixed `B` lets the graph compiler and LLVM specialize:

- loop trip counts;
- history/latency storage decisions;
- unrolling/vectorization profitability;
- scratch sizes/alignment;
- internal region subdivisions;
- constant index calculations.

In particular, decisions such as whether a required temporal carry `K` is smaller than or equal to the host block size become compile-time decisions.

### 15.3 Host block size versus region quantum

Some nodes/SCCs may require a maximum internal block size smaller than host `B`.

Distinguish:

```text
B
    host/kernel invocation size

Q(region)
    internal processing quantum required by max-block-size / feedback constraints
```

Example:

```text
B = 256
region Q = 64

kernel invocation:
    [0,64)
    [64,128)
    [128,192)
    [192,256)
```

`B` determines what state must survive between kernel calls. `Q` is a scheduling property inside the generated kernel.

---

## 16. Node execution interface and LLVM visibility

### 16.1 Preserve the conceptual C++ API

The current high-level node API is compatible with a much better execution model.

The preferred source style remains similar to:

```cpp
ctx.input<"x">(...);
ctx.output<"y">(...);
ctx.state();
```

or block variants using typed named ports.

The inefficiency today comes primarily from what those operations mean underneath: persistent runtime `InputPort`/`OutputPort` objects, shared buffers, cursors, conversion plans, wrappers, and erased callback boundaries.

### 16.2 Ports should express logical access, not physical storage

The generated kernel should interpret a port operation as a logical graph operation:

```text
read input port X at frame i/history h/channel c
write output port Y at frame i/channel c
```

not as:

```text
load InputPort object
load SharedPortData
load cursor
wrap ring index
branch on layout
load sample
```

This is what gives the finalizer freedom to implement a connection as SSA, scratch, history storage, a ring, or an external buffer.

### 16.3 Compiler-recognizable semantic port operations

A practical implementation is for the module/node LLVM to contain recognizable internal calls for logical port access. They do not need to be real LLVM intrinsics or require an LLVM fork.

Illustratively:

```llvm
%v = call float @iv.sample.read(... logical port ..., %frame, %history)
call void @iv.sample.write(... logical port ..., %frame, %v)
```

The whole-graph finalizer replaces/lowers those operations after graph wiring and storage choices are known, before final optimization.

This gives cached node implementation LLVM a stable graph-independent port vocabulary.

### 16.4 Erased callbacks should disappear from realtime execution

The current runtime path includes layers such as:

```text
GraphSccWrapper
    -> GraphNodeWrapper
        -> erased tick function pointer
            -> typed trampoline
                -> Node::tick / tick_block
```

The whole-graph compiler should use registered compiler metadata only to locate the LLVM implementation. It then generates direct calls and allows inlining.

After graph-specific optimization, ordinary hot-path code should not retain:

- `NodeCompilerRecord` lookups;
- `ReflectedNodeTickContext` adaptation;
- `GraphNodeWrapper::tick` indirection;
- generic port cursor objects;
- conversion function pointers.

### 16.5 Immutable node config and typed state

The node remains an immutable configured configuration object.

Multiple instances of one registered node type share one implementation function but have different constant/config operands.

State should lower to direct typed state storage known by the graph compiler rather than repeatedly treating state as an untyped byte span in the hot path.

This gives LLVM ordinary field-addressing and alias information after inlining.

---

## 17. Loop fusion and node scheduling

### 17.1 Consecutive `tick()` nodes should share the outer block loop

For simple acyclic sample-wise nodes:

```text
A -> B -> C
```

the desired generated shape is:

```cpp
for (size_t i = 0; i != B; ++i) {
    auto a = A_tick(...);
    auto b = B_tick(..., a);
    auto c = C_tick(..., b);
    output[i] = c;
}
```

rather than:

```text
loop B times for A -> materialize block
loop B times for B -> materialize block
loop B times for C -> materialize block
```

This exposes cross-node constant propagation, common subexpressions, dead stores, register/SSA forwarding, and SIMD opportunities.

### 17.2 Block nodes remain block operations initially

Nodes that explicitly implement block algorithms should retain their block-level execution semantics unless analysis proves a safe transformation.

The first whole-graph compiler does not need to solve arbitrary loop interchange/fusion across every possible block algorithm.

### 17.3 Dynamic-port algorithms remain dynamic

A node using dynamic untyped port loops has genuinely dynamic port selection. The compiler should preserve correctness rather than pretending every dynamic access can become a static named access.

The typed API exists specifically to give the compiler the stronger facts when they are true.

---

## 18. Stream storage: history, latency, lifetime, and reuse

### 18.1 Core principle

A connection/sample stream is a logical time-indexed value sequence.

**Storage is a lowering decision, not part of the connection's semantic identity.**

Do not begin by assuming either:

```text
connection = ring buffer
```

or:

```text
connection = SSA
```

The whole-graph schedule/use analysis chooses the appropriate physical representation.

### 18.2 Separate current-block values from persistent temporal carry

The current ring-buffer model makes history easy but can pin an entire edge allocation for its whole lifetime, preventing scratch reuse.

Instead, for many streams use:

```text
current block
    -> ephemeral/reusable scratch or SSA

past samples required by future kernel calls
    -> compact persistent carry
```

Let `K` denote the number of past samples that must survive across kernel calls for the stream, based on latency/history/feedback uses.

If `K == 0`, no persistent sample storage is required.

If `0 < K <= B`, a useful strategy is:

```text
current[B]            reusable scratch
carry[K]              persistent graph state

process consumers
copy only final K relevant samples into carry
release/reuse current scratch
```

On the next call a logical block/history view may span:

```text
carry prefix + current block suffix
```

without concatenating/copying a full block.

### 18.3 Large history/latency uses a cost model

When `K` is large, continuously shifting/copying a large carry may be worse than a persistent ring.

Initial physical strategies should include at least:

```text
SSA / scalar forwarding
reusable block scratch
compact carry + current scratch
persistent circular storage
external I/O storage
explicitly materialized contiguous block
feedback/SCC storage
```

The crossover between compact carry and a ring should be benchmarked rather than fixed philosophically.

Later, long block-aligned delays may benefit from block ownership rotation rather than copying.

### 18.4 Scratch memory should be reused globally

Once a current block's last consumer has run, its scratch allocation can be reused by a later disjoint part of the graph.

The storage planner should calculate live intervals for materialized streams and allocate static offsets in a preallocated graph scratch arena.

This is analogous to register allocation at block-buffer granularity.

Do not require machine-stack allocation for all block scratch; graph-wide scratch may be too large. The important property is that allocation is static/offline and no realtime heap allocation occurs.

### 18.5 Some connections need no memory at all

A simple fused acyclic chain may be represented entirely through LLVM SSA values/registers.

Fanout does not automatically require copies: one produced SSA value may feed multiple consumers.

Channel conversion does not automatically require a conversion buffer: it may become arithmetic at the consuming use.

### 18.6 Block views should avoid forcing contiguity

A default logical block view should be able to represent segmented storage, for example:

```text
persistent history prefix
+
current scratch suffix
```

or two portions of a ring.

The node-facing block API should not make `.data()`/contiguous memory an unavoidable semantic promise if the algorithm merely indexes/iterates the block.

For algorithms that genuinely require contiguous memory (FFT/native DSP library/etc.), provide an explicit materialization operation/contract. That tells the storage planner a real buffer is required at that point.

---

## 19. SCCs, feedback, detach, and region scheduling

### 19.1 SCC detection is whole-project work

SCCs cannot be cached as an iv-module-local execution fact because cross-module project connections can create a new cycle involving otherwise acyclic modules.

Therefore SCC detection runs only after all module instances and project connections have been combined.

### 19.2 Feedback determines real temporal storage

Feedback and history are cases where persistent storage is semantically real rather than an artifact of the generic runtime.

The whole-graph compiler determines:

- feedback latency;
- required persistent samples;
- scheduling quantum;
- whether a region can remain fused;
- what state must survive between sub-iterations and kernel calls.

### 19.3 Detach semantics must be preserved

The current `ConfiguredGraph` retains detach declarations. The new compiler must preserve their behavior even if the generated execution representation differs completely from today's generated routing/runtime nodes.

Detach should be treated as execution semantics that influence dependency/scheduling/storage analysis, not as a reason to keep the current runtime object model.

### 19.4 Max block size belongs in region scheduling

Registered node definitions expose their maximum block-size requirements. Once the full graph and host `B` are known, finalization partitions/schedules regions with the required `Q(region)` and generates the corresponding inner loops.

This should be visible in the debug execution plan so surprising subdivisions can be diagnosed.

---

## 20. TTL and activity

### 20.1 Current problem

The current `GraphSccWrapper` contains generic dormancy bookkeeping and repeatedly inspects sample/event blocks to determine whether inputs are unchanged and outputs are silent.

This can require O(block-size) scanning and stores per-node arrays for:

- dormant state;
- unchanged-input state;
- accumulated silent samples;
- effective TTL;
- remembered constant input values/valid bits.

The new compiler should not reproduce this generic discovery machinery in generated code.

### 20.2 TTL becomes an activity/quiescence contract

The existing TTL/skip declaration should seed a compiled activity model.

Conceptually, for a node whose input activity is known:

```text
input/event activity?
    yes -> run node, refresh/adjust tail state
    no but TTL tail remains -> run as required, decrement tail
    quiescent -> skip
```

Internal graph edges should propagate compact activity information rather than making every consumer scan an audio buffer to rediscover that the producer is quiescent.

### 20.3 Static elimination first

Before generating any runtime TTL branch, perform stronger compile-time analysis where possible:

- unreachable nodes/subgraphs -> DCE;
- statically inactive branch -> remove;
- constant graph portion -> fold/specialize;
- known permanently active source -> remove redundant activity branch.

Runtime activity state should exist only where project behavior is genuinely dynamic.

### 20.4 External sources are different

A true external audio input may still require runtime silence/activity detection if the project semantics depend on it, because the graph compiler does not control that producer.

Do not generalize that boundary cost to every internal connection.

---

## 21. SIMD and target specialization

### 21.1 `-O3` is helpful but not sufficient as a design

LLVM O3 enables aggressive optimization and vectorization pipelines, but it does not guarantee SIMD for every important loop.

Vectorization depends on:

- target CPU/features;
- legality/dependence analysis;
- alias information;
- alignment;
- unit-stride access;
- loop shape;
- profitability;
- control flow;
- calls/inlining.

The graph compiler should generate optimizer-friendly loops rather than rely on O3 to reverse-engineer graph structure from buffers and indirect calls.

### 21.2 Target CPU/features are part of kernel specialization

The current final native object emitter uses a generic target CPU for its `TargetMachine`. The new project kernel should instead use the actual host CPU/features by default for local execution, or an explicitly configured target.

Target triple, CPU, and feature string belong in the native-kernel cache key.

### 21.3 Generate appropriate graph loops first

For sample-wise regions, IV should generate the shared outer frame loop itself.

LLVM then optimizes/vectorizes a direct loop with:

- known `B`;
- known port/channel mappings;
- direct/inlined node implementations;
- explicit scratch/state addresses;
- minimal alias ambiguity;
- no dynamic conversion callbacks;
- segmented rather than modulo-heavy history accesses where possible.

### 21.4 Do not jump immediately to explicit vector IR

Start with clean scalar/loop LLVM and measure generated code/optimization remarks.

Only introduce explicit vector operations, stronger loop metadata, or specialized kernels where profiling/remarks show that standard LLVM optimization misses a meaningful case.

---

## 22. State layout and lifecycle

### 22.1 Runtime state is per concrete project node instance

Node implementation code is shared by type, but mutable `State` belongs to each concrete node instance in the finalized project graph.

The execution plan assigns persistent state storage for:

- node states;
- compact history/latency carry;
- persistent rings where chosen;
- feedback state;
- activity/TTL counters where required;
- other long-lived execution data.

### 22.2 Lifecycle plan is generated; lifecycle runs in the host

The finalizer has enough information to generate a lifecycle/state-layout description for the new kernel generation.

Actual migration must happen in the live application because it owns the old runtime state.

On generation replacement:

```text
same stable node + compatible state
    -> move/migrate

new node
    -> initialize

removed node
    -> release
```

Do not execute live-state `move()` inside the build/finalizer process.

### 22.3 Node type registration owns lifecycle implementation

Lifecycle code belongs to the independently registered node type, not to every iv module that instantiates it.

The finalizer/activation machinery resolves lifecycle operations from the node-type registry.

### 22.4 Do not freeze the final state ABI prematurely

State layout/migration remains essential, but the exact final runtime ABI should be explored alongside the generated execution model.

The existing reflected/runtime operation tables are migration bridges, not constraints that the new project kernel must preserve.

---

## 23. Cache and invalidation model

A central design requirement is that every stage has an explicit invalidation boundary.

### 23.1 Registered node type change

The compatibility reload policy is deliberately conservative. A change to a
registered primitive node queues the declared package set, then reconfigures
the definitions which use that ID:

```text
provider package changes
    -> reload package registry candidates
    -> reconfigure consumers
    -> replace their current ConfiguredGraphs
    -> invalidate future project finalization
```

### 23.2 IV module definition change

If the C++ graph-building implementation changes:

```text
recompile/re-author that IV package as necessary
replace that module's cached ConfiguredGraph
invalidate finalized projects containing instances of it
```

A parent iv module that instantiates `g.node<"child">()` is re-configured with
the declared package set. Artifact reuse is an implementation optimization;
the product rule is simply that a provider change reloads consumers.

### 23.3 Project connection edit

A project connection edit should invalidate only whole-project work:

```text
reuse:
    node type artifacts
    IV package/interface caches
    cached ConfiguredGraphs

redo:
    project connection resolution
    whole-project lowering
    execution plan
    kernel LLVM optimization/codegen
```

No Clang and no iv-module configuration JIT should run solely because the user rewired the project.

### 23.4 Module instance add/remove

If all required definitions are cached, adding/removing a module instance is also a whole-project finalization change. It does not rebuild the definition itself.

### 23.5 Block size/sample rate/CPU change

These invalidate kernel specialization/native code, not source or configured definitions.

Whether sample rate belongs in the kernel key depends on whether generated/registered code specializes on it. The safe initial design includes it.

### 23.6 Runtime control/state changes

Values intentionally modeled as live runtime controls/state must not require recompilation.

If a value is instead intentionally baked into immutable node configuration so LLVM can specialize it, changing that value invalidates the project kernel but not C++ source compilation.

### 23.7 Metadata-only changes

Source annotations, presentation hierarchy, tags, query metadata, and similar non-execution information should not invalidate a project kernel unless some compiler behavior actually consumes them.

### 23.8 Summary table

| Change | Rebuild node type? | Rebuild consuming IV package? | Re-author iv module? | Re-finalize project kernel? |
|---|---:|---:|---:|---:|
| registered primitive-node change | yes | yes, declared set | yes, users | yes |
| registered iv-module change | no | yes, declared set | yes, users and dependents | yes |
| project connection | no | no | no | yes |
| add/remove module instance | no | no | no | yes |
| immutable specialization config | no | no | no | yes |
| block size | no | no | no | yes |
| CPU/features | no | no | no | yes |
| live parameter/state value | no | no | no | no |
| presentation/source metadata only | no | normally no | normally no | no |

---

## 24. Whole-project compiler stages

The following is the consolidated stage model.

### Stage 1 — IV package compilation

Input:

```text
C++ source(s)
custom/default CMake
no provider implementation headers
```

Work:

- Clang frontend;
- source plugin registration/introspection;
- LLVM generation for node implementations defined by this IV package;
- compiler metadata/records;
- retained globals needed by configured configurations.

Output candidates:

```text
registered primitive node definitions
registered iv-module configuration functions
optional fresh static-interface metadata
retained source compiler data
```

### Stage 2 — IV-module configuration

The server assembles every valid compiled source artifact into one configuration
generation. For each registered iv module invocation:

```text
run builder function through the shared configuration generation
    -> ConfiguredGraph
```

`g.node<Id>` resolves the current registration and greedily realizes primitive
nodes or recursively executes iv-module configuration entries. The completed graph
contains no unresolved registered-ID boundary.

### Stage 3 — source registry validation/publication

The server combines the candidate definitions from all IV packages and checks:

```text
ID collisions
missing dependencies
iv-module dependency cycles
public-interface validity
```

On success, publish a new registry generation.

On failure, retain the previous valid live generation.

### Stage 4 — project input assembly

Collect:

```text
active project module instances
cached ConfiguredGraphs
project connections
registered node type definitions
```

No module has yet been flattened merely because another module uses it.

### Stage 5 — project connection and port resolution

Resolve project connections through:

```text
module instance
virtual node identity
direct-member selection if any
port/channel
```

Preserve dangling project connections if the stable identity is absent.

### Stage 6 — whole-project graph lowering

Now flatten execution-only structure as appropriate:

- virtual/tiled/subgraph implementation structure;
- current generated routing concepts;

while preserving stable identities separately for lifecycle/debugging/reconciliation.

Combine every configured and project connection so full producer/consumer knowledge exists.

### Stage 7 — connection lowering

Replace generic `ConnectionNode` execution with direct connection semantics:

```text
fanin
fanout
channel conversion
default values
runtime/project inputs
```

No physical buffer choice yet unless semantics force one.

### Stage 8 — dependency/SCC/region analysis

Compute:

- dependency graph;
- SCCs and feedback;
- detach semantics;
- max-block-size constraints;
- region processing quanta;
- observable outputs/side effects;
- graph DCE opportunities.

### Stage 9 — temporal/activity analysis

Compute:

- history requirements;
- latency requirements;
- persistent temporal carry `K`;
- TTL/quiescence behavior;
- activity propagation;
- cross-kernel-call lifetime.

### Stage 10 — storage planning

Choose for every logical produced value/stream:

```text
SSA
scratch
compact carry
ring
feedback storage
external I/O
explicit materialization
```

Run liveness analysis and assign reusable scratch offsets.

### Stage 11 — state/lifecycle planning

Assign persistent graph state layout for:

- node `State`s;
- temporal storage;
- feedback storage;
- activity state;
- other persistent execution data.

Produce migration/lifecycle metadata.

### Stage 12 — LLVM graph generation

Generate:

- direct graph loops;
- region loops with fixed `B`/`Q`;
- direct node implementation calls;
- connection arithmetic/conversion;
- concrete logical port accesses;
- state/scratch accesses;
- TTL/activity branches;
- external I/O accesses.

Import/reference unique registered node implementation LLVM only once per type revision.

### Stage 13 — graph-specific LLVM optimization

Initially use a correctness-first aggressive pipeline, then profile and narrow as justified.

Important transformations include:

```text
inlining
constant propagation
SROA/mem2reg
InstCombine/GVN
DCE
loop simplification
loop vectorization
SLP vectorization
```

### Stage 14 — native code emission and publication

Emit the project kernel/native artifact for the selected target CPU/features.

Activate only after successful compilation and state migration preparation.

Keep the previous generation active on failure.

---

## 25. Profiling and compiler visibility

This work should be developed with first-class visibility into every derived representation.

### 25.1 Debug output directory

Add an option conceptually like:

```text
--kernel-debug-dir=<path>
```

and emit stable stage files such as:

```text
00-project-graph.txt
10-resolved-graph.txt
20-connections.txt
30-sccs-regions.txt
40-temporal-analysis.txt
50-storage-plan.txt
60-state-layout.txt

70-llvm-generated.ll
80-llvm-after-port-lowering.ll
90-llvm-after-inline.ll
100-llvm-optimized.ll
110-kernel.s

optimization-remarks.yaml
timings.txt
metrics.txt
```

Exact numbering/names can evolve, but unoptimized and optimized LLVM must always be easy to inspect.

### 25.2 Timing metrics

Record at least:

```text
project_graph_resolution_us
connection_lowering_us
scc_region_analysis_us
temporal_analysis_us
storage_planning_us
state_layout_us
llvm_generation_us
llvm_inline_us
llvm_optimization_us
object_emit_us
native_link_or_publish_us
```

This should support a script analogous to the existing module-reload profiler.

### 25.3 Structural metrics

Useful per-kernel metrics include:

```text
nodes
connections
SCCs
regions
logical sample streams
SSA streams
scratch streams
persistent streams
peak scratch bytes
persistent temporal bytes
predicted sample copies/block
materializations
connection conversions
indirect calls remaining
LLVM instruction count pre/post optimization
loads/stores/calls/branches post optimization
vector instructions / vectorized loops
```

### 25.4 LLVM optimization remarks

Capture passed/missed/analysis remarks for important optimization families, especially:

- inlining;
- loop vectorization;
- SLP vectorization;
- loop transformations.

The development loop should answer “why did LLVM not vectorize this?” from compiler output rather than guess from timing alone.

### 25.5 Structural compiler tests

Avoid giant brittle textual LLVM snapshots as the main tests.

Instead assert invariants for small benchmark graphs, for example:

**Simple fused chain**

```text
one graph frame loop
no indirect tick call
no GraphNodeWrapper in hot path
no SharedPortData
no connection memcpy
```

**Fanout**

```text
producer value calculated once
no copy solely because two consumers exist
```

**K=4, B=256 history**

```text
persistent sample storage approximately 4 samples/channel
no full persistent edge ring unless planner explicitly chooses it
```

**Channel conversion**

```text
no intermediate conversion buffer where inline arithmetic is legal
```

---

## 26. Migration sequence

The design is intentionally staged so the existing 443-test runtime can remain the correctness oracle.

### Phase A — registration and source vocabulary

1. Introduce IV package as the loader-level concept without necessarily renaming all files/classes immediately.
2. Add stable registration IDs for node types and iv modules.
3. Build the server registry with transactional source updates and collision checking.
4. Preserve node implementation LLVM independently from iv-module definitions.
5. Allow one IV package to register any number of nodes/modules, including node-only sources.
6. Add cycle detection for iv-module ID dependencies.

### Phase B — ID-based GraphBuilder API

1. Add generic `g.node<Id>(...)` without generated provider headers.
2. Resolve registered IDs in a shared configuration generation and greedily embed iv modules.
3. Preserve existing global-pointer configuration relocation.
4. Transition reusable cross-source nodes away from `g.node<T>()`.
5. Keep the package-facing result dynamic: registered IDs always return `NodeRef`.

### Phase C — retain configured iv modules

1. Retain a lossless `ConfiguredGraph` per published iv-module definition.
2. Ensure all information necessary for virtual-node/member port addressing survives the cache.
3. Keep node implementations out of the configured-graph cache.
4. Keep any required source-owned retained globals/compiler data available for symbolic configured values.
5. Use conservative package reload while profiling determines whether a more selective cache is worthwhile.

### Phase D — project connections and multi-graph finalizer input

1. Add persistent project connections addressed through module instance + virtual node + direct member + port/channel.
2. Extend finalization to accept many module instances/`ConfiguredGraph`s plus project connections.
3. Resolve stable ports against the current configured definitions.
4. Preserve current runtime behavior after resolution so this boundary can be tested before the execution rewrite.

### Phase E — first whole-graph LLVM execution subset

Start deliberately small:

```text
fixed B
acyclic sample graph
known static port layouts
identity layouts
no events
no feedback/detach
no history/latency
ordinary tick() nodes
```

Generate consecutive sample-wise nodes inside one outer block loop and compare against the existing runtime.

Make unoptimized/optimized LLVM and assembly visible immediately.

### Phase F — direct connection lowering

Translate current `ConnectionNode` semantics into generated connection arithmetic/wiring.

Test:

- fanin;
- fanout;
- defaults;
- conversions;
- runtime/project contributions.

Eliminate temporary gather/conversion/copy buffers where not semantically necessary.

### Phase G — history, latency, and storage reuse

1. implement `K <= B` compact carry + reusable scratch;
2. implement scratch liveness/reuse;
3. benchmark large `K` against persistent ring storage;
4. add segmented logical block views;
5. add explicit contiguous materialization when required.

### Phase H — SCCs, region scheduling, TTL

1. whole-project SCC detection;
2. feedback/detach storage;
3. region/max-block-size scheduling;
4. compiled activity propagation;
5. TTL tail state;
6. remove internal O(block-size) dormancy scans where activity is already known.

### Phase I — SIMD/target tuning

1. compile for host/configured CPU features;
2. collect LLVM optimization remarks;
3. improve loop shape/alignment/alias facts;
4. only add explicit SIMD/vector-specific lowering where measurement justifies it.

### Phase J — events and remaining runtime semantics

Bring event routing, typed event ports, event storage, lifecycle corner cases, and remaining runtime behaviors into the new whole-graph execution model using the differential test harness.

### Phase K — lane/DSP graph convergence

Only after the kernel compiler works below the existing graph source, replace lane/per-module execution graph production with the canonical project graph described by the unified-graph direction.

Patch connection production into the same whole-project connection/lowering path.

Do not rewrite the kernel architecture merely because the graph's product/UI producer changes.

---

## 27. Differential correctness harness

Before replacing the current runtime, build a reusable harness that can execute the same configured/project graph through:

```text
existing RuntimeGraphRoot path
        vs
new generated whole-graph kernel
```

Feed identical inputs/events/state and compare outputs block-by-block.

Important coverage includes:

- fanout;
- channel layouts/conversions;
- public inputs/defaults;
- events;
- detach;
- feedback SCCs;
- TTL/dormancy/skipping;
- varying host block sizes;
- max block-size subdivisions;
- lifecycle/state migration;
- nested subgraphs;
- virtual/tiled-node connections;
- cross-module project connections;
- module reload with stable virtual identities.

The existing test suite is a behavioral specification. The new kernel does not need to preserve obsolete runtime structures, but it must preserve relevant product semantics.

---

## 28. Strong decisions

The following are treated as strong architectural decisions unless implementation reveals a contradiction.

1. **Clang/LLVM only.** No GCC fallback/back-compat path.
2. **One C++ frontend pass per changed IV package/TU.** Reuse LLVM for configuration and implementation extraction.
3. **`ConfiguredGraph` is the initial iv-module cache object.** Do not cache `BuilderSession`.
4. **`ConfiguredGraph` remains lossless.** Do not discard virtual/tiled/subgraph/addressability information merely to make lowering simpler.
5. **Node types are registered independently from iv modules.** Primitive implementation LLVM belongs to the node-type registry.
6. **Stable string IDs identify registered graph nodes.** An ID may be implemented by a primitive node type or an iv module.
7. **The normal cross-source configuration API is `g.node<"id">(...)`.** Callers do not name implementation C++ types.
8. **Registered-ID construction is always dynamic.** `g.node<"id">(...)` returns `NodeRef`; no generated static `node_interface<Id>` contract exists.
9. **IV packages may register many nodes and iv modules.** A node-only IV package is valid; experimental inline definitions remain convenient.
10. **Same-TU registered IDs are immediately usable.** The generic dynamic API does not rely on a second save/build or a generated `node_interface` specialization.
11. **Project cross-module connections use stable virtual-node/direct-member port identity, not concrete configured node IDs.**
12. **Project connections do not mutate cached `ConfiguredGraph`s.**
13. **Iv-module references are greedily expanded during configuration.** A completed `ConfiguredGraph` contains realized ordinary graph structure and no fake registered-node boundary.
14. **Iv-module dependency cycles are rejected transactionally.** Keep the previous valid live generation.
15. **Module/UI boundaries are not optimizer boundaries.** Final execution decisions use the whole active project graph.
16. **Block size `B` is a project-kernel specialization parameter.** It is not part of registered node identity or C++ source compilation.
17. **Connection storage is not predetermined.** Logical connections are lowered to SSA/scratch/carry/ring/etc. after schedule/use analysis.
18. **History/latency do not automatically imply a permanent full-edge ring.** Compact carry + reusable scratch is a first-class strategy.
19. **Consecutive sample-wise tick nodes should share graph-level outer loops when legal.**
20. **TTL/activity should be compiled from graph knowledge rather than rediscovered by scanning every internal audio block.**
21. **Global-pointer configuration relocation remains supported.**
22. **The finalizer generates lifecycle/state migration plans; the live host executes migration.**
23. **Profiling and LLVM visibility are first-class.** Every important whole-graph compiler stage should be dumpable and timed.
24. **Lane graph deletion is later and orthogonal.** The new execution compiler should be reusable when the canonical project graph replaces the current producer.

---

## 29. Deliberately unresolved points

The following should remain open until prototypes or profiling provide evidence.

### Registration/compiler implementation

- exact macro expansion for `IV_NODE` / `IV_MODULE`;
- diagnostics and introspection metadata for package registrations.

### Public arguments/configuration

- whether project-instance construction configuration becomes a product feature;
- if it does, how that persisted state is represented without exposing arbitrary
  provider C++ constructors;
- how general pointer/reference values beyond retained LLVM globals should be
  represented at that product boundary.

### Configured graph representation

- whether `ConfiguredGraph` itself remains the long-term persisted representation;
- whether a later lossless canonical form becomes worth caching closer to project lowering;
- how much current bundle/virtual-node data can be simplified without losing semantics.

### Whole-project lowering

- exact connection expression representation;
- exact region formation algorithm;
- cost model for compact carry vs rings vs block rotation;
- representation of logical block views/materialization;
- exact activity/TTL contract for arbitrary user nodes;
- event lowering/storage representation;
- amount of graph-specific inlining to perform versus retaining node calls;
- precise final optimization pipeline for fastest O3-quality kernel generation.

### Kernel cache

- whether native project kernels are persisted across application sessions;
- whether any whole-project intermediate becomes expensive enough to cache;
- exact hash granularity for immutable node config/global dependencies;
- whether node implementation LLVM is cached before or after particular reusable optimization passes.

### Future project graph

- exact API/data model after lane/DSP convergence;
- how direct project-created primitive nodes and iv-module-managed graphs coexist in persistence/reconciliation;
- how reification/subsumption integrate with registered IDs and cached configured graphs.

---

## 30. Near-term implementation target

The immediate design work should not begin with the new DSP kernel itself. The compiler inputs must first reflect the new ownership/cache model.

A practical first sequence is:

```text
1. independently registered node types
2. independently registered iv modules
3. IV package publishes multiple registrations transactionally
4. reusable source configuration artifacts and a shared configuration generation
5. generic `g.node<Id>()` with immediate registered-definition resolution
6. fully realized `ConfiguredGraph` invocation results and configuration-cycle checks
7. project connections through stable virtual-node/member ports
8. finalizer accepts the complete active project graph
9. only then begin replacing the generic runtime with whole-graph LLVM execution
```

This ordering is important because the whole-graph execution compiler should be built on the representation that will actually drive future project reloads. Otherwise execution work risks being optimized around per-module boundaries that the cache/registry design immediately removes.

---

## 31. Final architectural picture

```text
                              IV SOURCES
                     independently watched/built
                                |
                +---------------+----------------+
                |                                |
                v                                v
        IV_NODE("id", T)                 IV_MODULE("id", fn)
                |                                |
                +---------------+----------------+
                                |
                                v
                  reusable source configuration artifacts
                                |
                                v
                    shared configuration generation
                                |
                  g.node<"id">() resolves and authors
                                |
                +---------------+----------------+
                |                                |
                v                                v
       NodeTypeDefinition                  ConfiguredGraph invocation
        state + LLVM                     realized subgraphs only
                |                                |
                +---------------+----------------+
                                |
                                v
                         SERVER REGISTRY
                         stable ID -> definition
                                |
                    generic g.node<"some.id">(...)
                    dynamic NodeRef result


                            PROJECT STATE
                                |
            +-------------------+-------------------+
            |                                       |
            v                                       v
       module instances                     project connections
                                            virtual node/member
                                                + port/channel
            |                                       |
            +-------------------+-------------------+
                                |
                                v
                      WHOLE-PROJECT FINALIZER
                                |
                    resolve stable project ports
                    combine all connections
                                |
                                v
                         complete graph
                                |
                    connection lowering
                    SCC / feedback analysis
                    region / max-block schedule
                    history / latency analysis
                    TTL / activity analysis
                    storage liveness / reuse
                    state / lifecycle layout
                                |
                                +--------------------------+
                                |                          |
                                v                          v
                    registered node LLVM          source-owned globals
                                |                          |
                                +------------+-------------+
                                             |
                                             v
                                     graph LLVM kernel
                                     fixed B / CPU target
                                             |
                                     graph-specific O3
                                     vectorization / DCE
                                             |
                                             v
                                      native generation
                                             |
                               state migration + publication
                                             |
                                             v
                                      realtime execution
```

The key architectural split is now short enough to state directly:

> **IV packages compile definitions. `ConfiguredGraph`s cache iv-module configuration. The node-type registry owns primitive implementation code. Project connections address stable virtual-node/member ports. The finalizer is the first place all active graphs meet, and only there are scheduling, connection wiring, temporal storage, buffer reuse, TTL, and LLVM execution decisions made.**

That is the foundation for both fast whole-project graph reload and the later unified project graph.
