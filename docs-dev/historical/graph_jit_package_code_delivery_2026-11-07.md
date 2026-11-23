# GraphJit Package Code Delivery

_Historical status: non-normative. This file preserves the first design and
implementation of per-revision package code (Front 16, item 2). `GraphJit`
compiled each revision's package code when the package pipeline delivered the
revision to it through a new event._

_Sources, preserved 2026-11-07, all written earlier the same day and never run
successfully:_
- _[Graph JIT Direction](../graph_jit_direction.md): one paragraph in the section on
  the two JITs, and the "Current implementation checkpoint"._
- _[implementation_fronts_direction.md](../implementation_fronts_direction.md),
  Front 16: the item-2 implementation status._

_Reason for replacement:_
- _The delivery event broke the
  [Event Propagation Tree Constraint](../event_propagation_tree_constraint.md).
  `PackageWatcher::refresh_pending_packages()` raised
  `iv_runtime_package_jit_batch_completed_event`, which
  `package_watcher_graph_jit_bridge` routed to `GraphJit`. The same refresh then
  published the revisions:
  `iv_runtime_package_refresh_event` → `PackageDefinitions` → `NodeDefinitions` →
  `ProjectGraph::handle_node_definitions_snapshot_changed()`. That handler enters
  `GraphJit` a second time for the same cause, through
  `iv_runtime_project_graph_graph_jit_requested_event`._
- _`LinkerEventModuleFrameScope` threw "event propagation re-entered module
  GraphJit". `ProjectGraph` restored its previous definitions snapshot, and the
  `PackageWatcherService` worker caught the exception and exited. No definitions
  loaded: the edit benchmark waited 30 minutes for each shape, and the app fails the
  same way. The tests did not see it, because the GraphJit fixture called
  `handle_package_jit_batch_completed()` directly, outside any source invocation._
- _The violation was a symptom of placing the compile in the wrong module.
  `GraphJit::Impl::package_code_` kept per-revision state keyed by `package_code`
  identity. Between the two entries it held code for revisions that
  `PackageDefinitions` had not accepted, and it needed `weak_ptr` expiry checks to
  stay valid. [Package Pipeline Architecture](../package_pipeline_architecture.md)
  already assigns this work to `PackageJit`, which returns "self-contained immutable
  package revision results". The documented package refresh flow
  ([event_flows/package_refresh.md](../event_flows/package_refresh.md)) reaches
  `GraphJit` only through `ProjectGraph`._

_Replacement (approved by the human co-author on 2026-11-07):_
- _`iv_package_finalize` writes the node-code object once per package build._
- _`ModuleLoader` loads it with the package._
- _`PackageRevision` carries the node-code tables to `GraphJit` inside the one
  request from `ProjectGraph`._

_See [Graph JIT Direction](../graph_jit_direction.md), the section on the two JITs,
and Front 16 in
[implementation_fronts_direction.md](../implementation_fronts_direction.md)._

## Preserved normative paragraph (graph_jit_direction.md, two JITs)

> The native DSP output of the first JIT is **not** the code-generation input to the
> second JIT. Primitive node code consists of each node type's normalized Tick, skip,
> and batch operations and their callback closures. It depends only on the accepted
> package revision, never on a graph edit. `GraphJit` therefore compiles it once per
> accepted package revision, with the project target, from the retained primitive node
> LLVM of that revision. This happens before any project compilation uses the
> revision.

## Preserved implementation checkpoint (graph_jit_direction.md)

> The application/compiler shell around whole-graph lowering has landed. `GraphJit`
> owns a persistent project `LLJIT` with independently releasable resources. That
> `LLJIT` compiles concurrently, so package code can be compiled while a project
> compilation runs.
>
> When the package pipeline delivers an accepted revision,
> `GraphJit::handle_package_jit_batch_completed()` compiles that revision's package
> code once into its own `JITDylib`, at O3 for the project target. Package code is
> every node record's callbacks plus the retained globals, exported under names
> unique to that compilation. The delivery is `iv_runtime_package_jit_batch_completed_event`,
> raised by `PackageWatcher` before the revision is published.
>
> A project compilation then does the following:
> - captures one exact `ConfiguredGraph`/`NodeDefinitionsSnapshot` generation;
> - resolves registered concrete nodes to the accepted package revisions that created
>   them;
> - looks up each revision's package code by `NodeCodeKey`, and resolves symbolic
>   configuration-pointer relocations to its exported retained globals;
> - declares the imported callbacks and globals in the project module, which contains
>   no package code;
> - verifies the generated project LLVM, optimizes it at O3 (without loop unrolling),
>   and materializes it synchronously into a generation `JITDylib`, linked against
>   the package code it pins;
> - returns an immutable `CompiledGraph`.
>
> This is implementation status as of 2026-11-07, awaiting the repository owner's
> build and test run.

## Preserved Front 16 implementation status

> *Package code compiled once per revision (implementation status, 2026-11-07; written,
> awaiting the repository owner's build, tests, and measurement).*
> - **Pipeline.** `PackageWatcher::refresh_pending_packages()` raises
>   `iv_runtime_package_jit_batch_completed_event` (payload `PackageJitBatchResult`)
>   after its synchronous `PackageJit` request returns and before
>   `iv_runtime_package_refresh_event`. `package_watcher_graph_jit_bridge` routes it to
>   `GraphJit::handle_package_jit_batch_completed()`. The app and the edit benchmark
>   bind that bridge.
> - **`GraphJit`.** `package_code_module()` reduces a revision's finalized package LLVM
>   to package code:
>   - it exports every node record's callbacks, type-checked against
>     `graph_jit::package_callback_type()`, and every retained global under
>     `__iv_package_<serial>_*` names;
>   - it erases `llvm.used` and `llvm.compiler.used`, internalizes everything else, and
>     removes the `target-cpu`, `target-features`, and `tune-cpu` attributes.
>
>   `compile_package_code()` then runs GlobalDCE and O3 with Clang's O3 tuning (loop
>   and SLP vectorization, interleaving, unrolling) on a dedicated `TargetMachine`. It
>   adds the result to its own package `JITDylib` and looks every export up, which
>   generates code immediately. `GraphJit::Impl::package_code_` keys the result by the
>   revision's `package_code` identity and also records failures.
> - **Locking.** The project `LLJIT` is built with
>   `setSupportConcurrentCompilation(true)`. A package compile holds
>   `SharedProjectJit::mutex` only while it creates the `JITDylib` and adds the
>   module.
> - **Compilation.** `compile_impl()` no longer parses package bitcode;
>   `GraphJitCompileTimings::package_code_us` replaces `package_parse_us` and
>   `package_bitcode_bytes`. A revision without package code is a `package_llvm`
>   diagnostic, never a lazy compile.
> - **Lowering.** `graph_jit::NodeImplementation` holds exported callback symbols and
>   a `graph_jit::PackageCode`. `plan_package_imports()` and `emit_package_imports()`
>   declare the imported symbols. Checks that required imported callbacks to be
>   definitions now require them to be imported.
> - **Materialization.** `materialize_project_module()` adds each package
>   `JITDylib` to the generation's link order. `ProjectCodeLifetime::package_code`
>   pins those `JITDylib`s until after the generation `JITDylib` is removed.
> - **Effects to measure.** Package code now runs for the host CPU instead of baseline
>   x86-64, with SLP vectorization. The per-edit project module contains only the
>   root and background operations, so its optimization and code generation should
>   fall to the root's own cost.
> - **Tests.** `GraphJitRuntimeFixture::deliver_package_code()` hands the fixture
>   revision to each `GraphJit` the fixture tests construct.
