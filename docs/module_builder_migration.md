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
- `basic_nodes/polyphonic.h` is part of the module API; `midi.h` no longer
  exists. The module PCH includes both `dsl.h` and `polyphonic.h`.

`dsl.h` still includes the node contract required to define a node in a module:
port configurations, traits, and declaration/initialization/move/release/tick
contexts. JUCE/VST support remains available through its explicit API.

## C-string node configuration

Node definitions do not implement a serialization, relocation, or validation
trait. The Clang metadata plugin discovers specializations of the internal
`iv::details::node_compiler_record<T>` variable template, which
`reflect_node<T>` instantiates for every node that actually enters a builder.
It records byte offsets of every `char const*` configuration field under the
build-local `NodeCodeKey`. The plugin emits the definitive metadata snapshot in
`EndSourceFileAction`, after CodeGen has completed deferred instantiations, so
the final AST and LLVM node-record set agree even for nodes materialized by
header-only builder helpers such as scalar lifting to `Constant`.
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
- The existing node-config materialization test still verifies alignment,
  empty strings, embedded NUL payloads, and duplicate relocation rejection.
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
