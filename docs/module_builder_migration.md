# Module Builder Migration

This is the durable implementation log for the compiler migration. It records
the intended boundaries and outstanding verification work across context
compaction.

## Goal

An IV module compiles only its DSL expressions, node definitions, typed
references, and source instrumentation. `GraphBuilder` is a small module-side
facade over the precompiled `iv_builder` library. The library owns the mutable
graph representation, connection and reference-expression tables, annotations,
public/virtual ports, detach bookkeeping, subgraph scopes, and graph finish.

The temporary ORC build generation never owns data retained by the finished
graph. `NodeCodeKey` joins a node configuration to metadata emitted by the
same translation-unit build only. It is not hot-reload identity; state reuse
continues to use the final runtime state-compatibility rules.

## Current structure

- `graph/builder.h` declares the module-facing `GraphBuilder` facade.
- `graph/builder/builder.cpp` implements its non-template forwards.
- `graph/builder/node_refs.h/.cpp` own reference declarations and non-template
  behavior. Only type-dependent reference operations remain in headers.
- `graph/builder/state.h/.cpp` are the private host graph state.
- `graph/builder/host.hpp` is the header-only test/host inspection surface.
- `module/builder_session.h/.cpp` define the opaque `BuilderSession` that owns
  `GraphBuilderState` for a build.
- `module/source_annotations.h/.cpp` are the narrow DSL instrumentation bridge;
  they are not part of `BuilderSession`.
- `node/config_storage.h/.cpp` own copied node-configuration bytes and their
  host-library deleter.
- `node/compiler_record.h` is the stable build-local record ABI consumed by
  the finalizer. `node/build_request.h` contains the small type-specific
  templates that make that record and enumerate one node value.
- `graph/reflected_node_description.h/.cpp` are host-only: the `.cpp` owns
  construction of port vectors and retained node descriptions. Module code
  never constructs `ReflectedNodeDescription` directly.
- `graph/node_ports.h` preserves the existing typed-reference return type
  without pulling retained graph storage into `node_refs.h`.
- `basic_nodes/polyphonic.h` is part of the module API; `midi.h` no longer
  exists. The module PCH includes both `dsl.h` and `polyphonic.h`.

`dsl.h` still includes the node contract required to define a node in a module:
port configurations, traits, and declaration/initialization/move/release/tick
contexts. JUCE/VST support remains available through its explicit API.

## Node-build request boundary

`GraphBuilder::node<T>` now constructs `T` and sends a compact
`NodeBuildRequest` to `iv_builder`. The request contains only a borrowed
configuration address and extent, the build-local compiler record, and a
type-specialized callback that enumerates port configs and node traits. The
builder library immediately copies the configuration, captures C-string
relocations, invokes the callback against the copied configuration, and owns
the resulting `ReflectedNodeDescription`.

This deliberately does not add a node trait, registration hook, or other
boilerplate to node definitions. A precompiled library cannot discover the
ports or lifecycle behavior of an arbitrary module-defined type by itself, so
the callback is the irreducible type-specific portion. Dynamic containers,
description assembly, retained storage, and validation remain in the library.

The module-facing reference path no longer reaches retained node bundles, and
shape traits include `node/traits.h` rather than the runtime `graph/node.h`.
Those are dependency cuts, not alternate public APIs. The broad module PCH is
still intentional and remains the baseline for all supported DSL, polyphonic,
and VST-facing declarations.

## C-string node configuration

Node definitions do not implement a serialization, relocation, or validation
trait. The Clang metadata plugin discovers specializations of the internal
`iv::details::node_compiler_record<T>` variable template, which
`make_node_build_request<T>` instantiates for every node that actually enters
a builder.
It records byte offsets of every `char const*` configuration field under the
build-local `NodeCodeKey`. The plugin emits the definitive metadata snapshot in
`HandleTranslationUnit` writes the initial metadata sidecar. If CodeGen later
materializes a deferred compiler record—such as scalar lifting to `Constant`—a
Clang variable-template-specialization notification refreshes that sidecar.
Both paths derive state and C-string metadata solely from actual compiler
records rather than traversing every record in the translation unit. Unrelated
types that happen to declare a nested `State` therefore cannot become
accidental node ABI inputs.
`PluginASTAction::EndSourceFileAction` is not a usable flush point for this
`AddBeforeMainAction` consumer: it does not retain the consumer at the required
post-CodeGen point. Do not move the sidecar write there without proving the
module-link finalizer can still observe generated metadata.
`node_compiler_record` is compiler plumbing, not a node-traits or module-facing
API. That includes ordinary non-virtual base classes, nested trivially-copyable
configuration records, and fixed arrays. It emits an empty layout for node
types with no C-string fields, so the finalizer can reject missing layout
metadata for any compiler record rather than silently leave a JIT pointer in
the finished graph. Before executing the module build function, the finalizer
installs those layouts into `BuilderSession`. When a node is inserted, the
precompiled builder copies each non-null C string while the temporary JIT
generation is still live, then records ordinary internal relocation data for
final module materialization. A null C-string pointer stays null.

This keeps raw `char const*` node configuration simple while ensuring dynamic
string storage does not point into an unloaded JIT generation.

## Tests added for the changed assumptions

- `GraphModules.BuilderCapturesCStringConfigurationFromCompilerFieldMetadata`
  provides two C-string field offsets to a session, builds a node with two
  dynamic strings, and verifies both copied relocation values in the serialized
  graph.
- `GraphModules.BuilderCapturesNestedAndArrayCStringConfiguration` covers the
  structural offset cases used by compiler metadata: nested configuration and
  a fixed array of C-string pointers.
- `GraphModules.BuilderRejectsAmbiguousCStringConfigurationLayouts` ensures
  malformed compiler-layout input cannot duplicate a relocation or select two
  layouts for one `NodeCodeKey`.
- `ModuleNodeConfiguration.CopiesCStringFieldsBeforeModuleBuildEnds` exercises
  the actual metadata-plugin → finalizer → module-loader path. The module
  mutates its direct, array, and nested source strings after `g.node`; the
  loaded configuration must retain every original value.
- `ModuleCompilerMetadata.CapturesImplicitConstantForScalarNodeInput` covers a
  node record introduced by an inline builder helper rather than a source-level
  `g.node<Constant>` call. A scalar input materializes `Constant` during
  `GraphBuilder::node<T>` template instantiation, and it must receive an empty
  configuration layout before finalization.
- `ModuleCompilerMetadata.IgnoresUnusedTypesThatOnlyResembleNodes` proves that
  State ABI validation follows actual `g.node<T>` compiler records rather than
  all AST records. An unused type deliberately has an invalid node-style State;
  the used node still receives complete state metadata and initializes normally.
- The existing node-config materialization test still verifies alignment,
  empty strings, embedded NUL payloads, and duplicate relocation rejection.
- `NodeBuildRequest.MaterializesHostOwnedDescriptionFromTypeSpecificCallback`
  verifies the new boundary directly: the copied configuration becomes the
  runtime callback data, while dynamic ports and trait values are assembled by
  the builder-owned description.
- The existing event-only functional-subgraph regression remains required:
  a functional scope may expose only event outputs; imported modules still
  require a sample boundary output.

## Required handoff verification

No build or test suite has been run in this edit pass by request. The next
build must regenerate the Clang metadata plugin and finalizer together, then
run the full test suite. The new module-level C-string test should exercise the
metadata-to-finalizer path rather than only the session-level behavior.

## Module build invalidation

The Clang plugin and finalizer are tools named only in compile/link command
lines, so CMake does not infer that changing either invalidates a persistent
module workspace. `ModuleSupport.cmake` makes the plugin an explicit object
dependency for every runtime-module source and makes the finalizer a link
dependency. Tests likewise keep their temporary modules, locks, and shared
fixtures below the active CMake binary directory rather than a hard-coded
repository `build/` directory. This prevents a release test binary from
reusing debug-tree fixture metadata or any stale module object after a tool
rebuild.

Debugging of the shared local-CMake fixture established the actual gap: LLVM
contained compiler records for both `SawOscillator` and the implicit
`Constant`, while the metadata JSON contained only `SawOscillator`. The prior
scan of `GraphBuilder::node<T>` function specializations missed the `Constant`
whose surrounding function specialization was materialized lazily during code
generation. Configuration discovery now uses the corresponding compiler-record
variable specializations instead. The module-level regression binds two named
numeric inputs, matching the fixture shape that exposed this error.

The generated `root_export.cpp` explicitly includes `module/abi.h`. It owns
the exported ABI symbols and therefore must not depend on an incidental DSL or
builder-header include to provide `IV_MODULE_EXPORT`; the module-build behavior
test asserts this include is present.

## Reload-performance follow-up

The next objective is not to remove normal standard-library facilities from
the DSL. The intentional target is less per-module frontend work and less IR
sent through final optimization. `std::span`, `std::string_view`,
`std::optional`, and ordinary traits are acceptable when they describe the
module contract directly.

Before another large architecture change, measure a source-only warm reload.
`iv_module_build_benchmark` now records the full build boundary: CMake
configure, Ninja, PCH/export/link edges, generation copying, and finalizer
sub-stages. The finalizer writes a replacement sidecar at
`cmake-build/iv-module-finalizer-timings.txt`; it contains microsecond timings
for bitcode parse/link, metadata, authoring-module clone, JIT creation and
materialization, `module_main`, graph serialization/injection, runtime O3,
native object emission, and the native link. `ModuleLoader` also reports
generation copying, dynamic-library loading, and runtime graph materialization
through its existing `LogSink`.

The focused entry point is `scripts/profile_module_reload.sh`. It builds only
the profiling executable and measures a cold build followed by a source-only
hot reload in an isolated, retained workspace. The broader
`scripts/verify_release_performance.sh` remains a release verification script:
it rebuilds, runs every release test, and then measures execution as well.

Baseline recorded on 2026-09-09 for the default `simple_sine/saw` module at
O3: hot end-to-end reload was 1.633 s. CMake configure was 18.6 ms, Ninja was
1.613 s, and the finalizer was 879.7 ms: 283.4 ms JIT materialization,
143.4 ms runtime O3, 215.2 ms native object emission, and 209.9 ms native
link. The export compile edge was 715 ms and the link edge 889 ms. Thus
unnecessary metadata-plugin traversal is the first measured frontend cut;
splitting configure invalidation is useful cleanup but not the immediate
hot-path priority.

After replacing whole-translation-unit metadata traversal with actual
compiler-record collection, the same workload measured 1.667 s hot: 691 ms
export compilation, 943 ms link/finalization, 21.8 ms CMake configure, and a
932.3 ms finalizer. The export edge improved from the earlier 715 ms, but the
full difference is within ordinary machine variation. The finalizer remains
the dominant cost: 305.5 ms JIT materialization, 156.6 ms runtime O3,
230.1 ms native object emission, and 209.5 ms native link. Do not spend more
time on CMake configure before these larger stages; the next module-frontend
cut remains thinning `node/layout.h`.

Do not begin the following work until the new report identifies the dominant
stage:

1. Remove low-value module-facing includes (`basic_nodes/arithmetic.h` from
   `graph/builder.h`, and the `Constant`-driven `type_erased.h` dependency from
   `node/build_request.h`) without introducing a node-traits hook or other
   node-definition boilerplate.
2. Separate port declarations used by node definitions from port runtime
   storage/buffer management where that does not weaken node `tick()` APIs.
3. The metadata plugin now snapshots actual `node_compiler_record<T>`
   specializations initially, then only when a deferred compiler record is
   materialized; confirm the next profile records the expected export-stage
   reduction before selecting another frontend cut.
4. Split module configuration identity from source/build identity: source-only
   edits should invoke Ninja without an unnecessary CMake configure, while
   changes to CMake, manifests, imports, include paths, toolchain, or generated
   source lists must still reconfigure.
5. After `module_main` has run, prune authoring-only IR before the runtime O3
   pass, preserving all symbols reachable from retained node compiler records.
   This requires a reachability test; it must not discard runtime callbacks.

Only compare O2/O3 or introduce IR/content caching after these measurements.
The future destination—specializing an authored graph into a real-time graph
kernel—is separate work and must not be mixed into this reload-cost reduction.

### Node-layout extraction

The completed extraction keeps the node-definition API unchanged:
`DeclarationContext<T>`, `InitializationContext<T>`, `MoveContext<T>`, and
`ReleaseContext<T>` remain typed facades. They retain the small amount of
template code that cannot be precompiled: State casts, span assignment lambdas,
and lifecycle callbacks for a module-defined `T`.

Everything else moves behind an internal untyped layout-operation boundary in
the builder library: NodeLayout records, state and array region allocation,
dependency ordering, exported-array lookup, storage initialization, migration,
and release. `node/layout.h` and `node/layout.cpp` remain the host pair;
`node/context.h` becomes the module-facing header included by `lifecycle.h`.
`build_request.h` must use the same boundary when applying finalizer-provided
`NodeStateStructure`, rather than requiring the complete host builder type.

`NodeLayoutBuilder` and `NodeStorage` implementation now lives in
`node/layout.cpp`, linked through `intravenous_graph_builder`. The builder
state, allocation, migration, and exported-array lookup are absent from the
module-facing header closure. State ABI values used by finalizer/archive code
live separately in `node/node_state_structure.h`; that data dependency does
not require layout or storage implementation. `ModuleContextBoundary.DslKeepsLayoutAndStorageIncomplete`
includes the actual `dsl.h` and statically verifies that neither host type is
complete there; this is the regression guard for the dependency cut.

Do not reintroduce the old implementation through a header-only operation
table. The point is both to remove `layout.h` from the module PCH closure and
to keep layout/storage independently compiled and debuggable.

## Runtime IR pruning

Finalizer-side IR pruning runs after the temporary builder JIT has run and
serialized the graph. The finalizer marks the exact
`iv_module_build` authoring closure, separately marks the runtime ABI entry
points and their retained node-record callback closure, drops authoring-only
entries from LLVM used lists, internalizes the remaining authoring-only
definitions, and runs `GlobalDCEPass` before O3. It fails finalization if
`iv_module_build` survives. The existing module-load tests exercise retained
runtime callbacks, while the finalizer timing sidecar reports
`authoring_ir_prune_us` so module build behavior verifies that this stage ran.
The standalone DCE step deliberately uses LLVM's legacy pass manager, which
is already used for object emission: the new pass manager requires explicit
analysis registration and is inappropriate for this one isolated legacy pass.

Verification: the release build and all 439 tests passed. A warm O3 profile
of the default `simple_sine/saw` module measured 1.371 s: 677 ms export
compilation, 666 ms link/finalization, 17.5 ms configure, and 657.2 ms
finalizer total. Its finalizer stages were 249.5 ms JIT materialization,
1.9 ms authoring-IR pruning, 72.2 ms runtime O3, 92.9 ms native object
emission, and 215.7 ms native link.

This is a 166 ms (10.8%) warm-reload reduction and a 171.9 ms (20.7%)
finalizer reduction relative to the prior post-layout-split warm sample
(1.537 s / 829.1 ms). The pruning work itself is negligible; the benefit
comes from giving O3 and code emission a smaller runtime-only module. This is
still one profile per configuration, not a controlled benchmark. Repeat the
same profiling script after the next cut before attributing a durable
improvement to any one change.
