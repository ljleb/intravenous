# LLVM Module Reload Migration Log

This is a durable working log for the Clang/LLVM module-reload migration. It
records decisions, observed failures, and verification so work can resume
without relying on conversation context.

## Scope and invariants

- Replace the deleted GCC reflection/`consteval` module pipeline with Clang 23,
  LLVM bitcode, an ORC authoring phase, and the existing runtime graph compiler.
- Preserve the existing `GraphLowerer -> GraphCompiler -> RuntimeGraphRoot`
  execution route for this migration; specialized LLVM graph kernels are later
  work.
- `NodeCodeKey` is an internal join key within one finalized module generation.
  It has no hot-reload meaning and must not become a cross-generation ABI.
- State metadata must be bound unambiguously to the compiler record for the
  same node type. A mismatch must fail module finalization rather than publish
  metadata that could permit an invalid state migration.
- Node configuration validation belongs at the `GraphBuilder::node` boundary,
  not in unrelated compilation phases.
- Variable-length node configuration needs explicit lifetime handling. A raw
  pointer copied from the ORC authoring process into finalized config bytes is
  never valid at runtime.

## Starting point: `dcd38d0`

The branch contains the source-level architecture: Clang-only top-level CMake,
the Clang source plugin, `iv_module_finalize`, raw authored-config wire data,
and loader reconstruction into the existing lowerer/compiler.

Before this work, state metadata was collected with both a node USR and a
printed node name, but `iv_module_finalize` discarded the USR and matched it to
compiler records by normalized printed name. That is not safe enough for state
migration or template/unusual-type cases.

The deleted frozen-graph API was still used by six host-test sources. Those
tests now use a test-only runtime-owned graph snapshot helper; production code
does not reintroduce the former frozen graph representation.

## Work plan

1. Build against the LLVM 23 Nix environment and resolve actual compiler/API
   diagnostics.
2. Carry a compiler identity from the Clang plugin to the LLVM record and bind
   state metadata directly; reject missing, duplicate, or inconsistent entries.
3. Move configuration validation to `GraphBuilder::node`, implement the
   permitted configuration contract, and migrate `DebugProbe` safely.
4. Replace frozen-graph test helpers with runtime-owned authored graphs.
5. Verify a trivial module end-to-end, then run module reload, source
   introspection, and full test suites.

## Verification log

- Environment repaired on Linux: the flake now targets the two Linux systems
  only, uses `nixos-unstable`, and resolves Clang/LLVM 23.1.0. The project
  `build/` directory was reconfigured in place with Nix's Clang 23.1.0, CMake
  4.3.4, and Ninja 1.13.2; the temporary `build-clang23/` directory was
  removed at the user's request.
- CMake's package discovery asks for LLVM/Clang without a component-version
  constraint, then explicitly verifies `LLVM_VERSION_MAJOR == 23`. LLVM
  23.1.0's CMake package otherwise rejects `find_package(LLVM 23 ...)` even
  though its major version is correct.
- The first host-tool build exposed an omitted JSON dependency on
  `iv_module_finalize` and three Clang-23 plugin API changes (record-layout
  definition, bit-field API, and record type lookup). Those corrections are
  complete.
- Completed: both `iv_clang_source_introspection_plugin` and
  `iv_module_finalize` build in the repaired Clang/LLVM 23 environment.
- Completed: metadata schema version 4 writes a Clang USR plus a two-part
  `NodeCodeKey` for each State record. The finalizer now reads the key directly
  and will require matching metadata and ABI size/alignment for every compiler
  record with `Node::State`; no display-name matching is permitted. State
  metadata collection is independent of the optional source-annotation mode.
- Completed: the plugin no longer validates node configurations. The supported
  baseline constraint is the trivially-copyable assertion at
  `GraphBuilder::node`.
- Valgrind investigation of a source-enabled module compile found a real stack
  overflow in Clang's record-layout query for a dependent template `State`
  (`MidiVoiceAllocator`). The plugin now skips dependent records; concrete
  specializations remain eligible for metadata. This changed the failing
  source-off compiler invocation from a stack overflow to success.
- The end-to-end finalizer path now gets through ORC authoring, State metadata
  binding, PIC native-object emission, and final shared-library linking for a
  focused module. LLVM target registration, the explicit `stdc++exp` archive,
  and PIC relocation were all required for that result.
- The focused source-enabled loader test then failed after linking, while
  decoding the published authored graph: `nlohmann::json` converted the
  default infinite `InputConfig` bounds into JSON `null`, while the decoder
  expected floats. JSON has now been removed from this module boundary.
  `authored_graph_binary_archive.hpp` writes a versioned native archive:
  scalar values retain their bytes (including infinities), and strings/ranges
  are sized values reconstructed into owning graph records at load time. The
  module ABI is version 8 to reject the old payload format and require the
  node-config string relocation table. A clean focused
  source-enabled run completed successfully after rebuilding the original
  module through source/CMake edits and a local-CMake module.
- Completed: `NodeConfigString` is a trivial
  `(char const*, size)` node-config field. `DebugProbe` uses it, preserving
  embedded-NUL-safe length semantics. Authoring collects the offset of its
  pointer field and its bytes. The finalizer emits immutable relocation records
  and string globals; the loader makes an aligned private copy of every node
  config, owns the string bytes, then patches pointers in that copy. This avoids
  both dangling ORC pointers and a writable relocation section in the module.
- Correctness fix during that work: a first implementation attempted to call a
  type-erased relocation collector while serializing after the ORC authoring
  JIT had been destroyed. GDB showed the callback target was therefore stale.
  Collection now happens in `reflect_node` while the typed node/JIT is alive;
  only owned `(offset, string)` relocation data passes through `NodeBundle` to
  the finalizer. The exact former crashing finalizer command then exited cleanly.
- Verification: the source/CMake behavior fixture now has a silent
  `DebugProbe` with label `"behavior probe"`. Its module compiles, finalizes,
  loads, source-rebuilds twice, and completes the independent local-CMake
  rebuild with a nonempty string-relocation table.
- Verification: `NodeStorageMigration` now covers the hot-reload safety case
  directly. With a replacement-generation type token, the same node identity,
  and equal State byte size/alignment, changing one field from `int` to
  `float` makes `can_move_from` return false. Preparing and committing that
  reload then initializes the new State and never calls its move callback.
- Completed: the six old host test sources and the compiler graph tests no
  longer depend on compile-time frozen graph values. `AuthoredGraphTestView`
  lives under `tests/`, owns an `AuthoredGraph` at runtime, and gives each
  lowering pass an independent copy. The former `consteval` compiler checks
  are now ordinary runtime tests. This deliberately keeps the old transport
  out of the production API.
- Completed: runtime reflection's `copy_authored_node_bytes` now belongs to
  the graph-builder object library, so both host tests and `iv_module_shared`
  resolve it. The module-execution benchmark now passes the native archive as
  `span<byte const>` rather than pretending it is a JSON string. The module
  behavior test accepts both CMake PCH file formats (`.pch` for Clang and
  `.gch` for GCC).
- GDB investigation of a State-bearing module found a second real lifetime
  issue. `deserialize_authored_graph` built a `NodeStateStructure` in a
  temporary record, and `ReflectedNodeRuntimeOperations::state_structure`
  still pointed at that record after `NodeBundle` took shared ownership. The
  callback therefore read freed vector metadata during `BlockNodeExecutor`
  layout creation and threw `std::bad_alloc`. `from_authored_records` now
  rebinds that pointer to the bundle-owned structure. The compiled `Graph`
  also retains all authored node configurations and State structures for the
  lifetime of its raw callback pointers, not only compiler-generated nodes.
- Verification: `ModuleBuildBehavior` now creates an executor from the
  finalized `behavior_project` module and requires the nested
  `SawOscillator::State::phase` field to arrive with a nonempty type USR.
  This proves the Clang plugin -> exact `NodeCodeKey` finalizer binding ->
  binary archive -> loader -> layout path. The same heavy test then completes
  the source and CMake rebuild cases successfully (about 32 seconds).
- Final verification: a complete Linux `cmake --build build -j4` succeeds,
  and `ctest --test-dir build --output-on-failure -L light` passes all 384
  light tests.

## Remaining work

1. Add a direct archive round-trip/corruption test for the native graph
   payload.
2. Add direct unit coverage for `NodeConfigString` relocation, including empty
   and embedded-NUL labels. The existing module behavior fixture covers the
   ordinary nonempty end-to-end case.
3. Add a two-generation module hot-reload test that changes a same-size State
   field type. The end-to-end test now proves compiler metadata reaches the
   layout, and `NodeStorageMigration` proves that differing field USRs reject
   a move; one test still does not compile both generations and exercise the
   full reload coordinator together.
