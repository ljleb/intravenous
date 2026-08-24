# Runtime PGO For Hot-Reloadable IV Modules

## Status

Discovery note only. This records a possible future feature; it does not commit
the project to a manifest, RPC, UI, compiler-flag, or runtime ABI yet.

## Intent

Allow a hot-reloadable iv-module to train on the workload it actually sees,
compile a profile-guided replacement in the background, and activate that
replacement without stopping execution or losing instance state.

The first useful interpretation of “gets faster as it runs longer” is one
promotion per exact module build identity:

```text
fresh source identity
  -> instrumented Release generation
  -> representative execution
  -> profile snapshot and merge
  -> profile-use Release generation
  -> ordinary hot-reload activation
```

The optimized generation does not need to remain instrumented. Continuous or
repeated re-optimization can be considered later, after a one-promotion design
has demonstrated a net benefit.

PGO is workload specialization, not a guarantee of improvement. A skewed
training workload can make a different workload slower, so the feature needs
measurement, diagnostics, and a conservative fallback.

## Why This Fits The Current Runtime

The current module lifecycle already provides most of the structural seams:

- `ModuleLoader` compiles a root definition and its reachable module closure
  into a shared library.
- Every load is copied to a unique generation path before `dlopen` /
  `LoadLibrary`, so an optimized sibling can coexist with its trainer.
- `IvModuleReload` already compiles away from the task-runner workers and
  publishes a completed definition later.
- `BlockNodeExecutor::prepare_reload()` constructs replacement graph state and
  prepares state migration before activation.
- task-runner bridges commit replacement graphs only at a pass boundary.
- `IvModuleInstancesExecution` retains `ModuleRef`s until the graph revision
  that used their callbacks is no longer reachable. The same lifetime rule can
  prevent a profiling runtime from being unloaded before its last profile is
  captured.

PGO should use those seams. It should not add compilation, profile file I/O, or
profile merging to a DSP task.

## Proposed First Scope

Start with an explicitly enabled, Clang-only, Release-only experiment:

- use Clang IR instrumentation (`-fprofile-generate`) for training;
- merge raw profiles with the matching `llvm-profdata`;
- build the promoted generation with `-fprofile-use`;
- optimize the root definition shared library, including the reachable IV
  module definitions compiled into it;
- aggregate training from all live instances executing the same binary
  generation;
- perform at most one automatic promotion for a build identity;
- preserve the existing reload failure behavior: the current working
  generation remains active when profiling, merging, loading, or promotion
  fails.

Clang documents IR instrumentation as the preferable instrumentation mode for
PGO performance and overhead. Frontend instrumentation
(`-fprofile-instr-generate`) is more appropriate when source-correlated
coverage is the primary goal.

The initial scope should not include:

- PGO of the Intravenous host or the prebuilt `iv_module_shared` library;
- Debug builds;
- GCC, MSVC, or cross-toolchain profile compatibility;
- libraries prebuilt outside the runtime-module target by a custom module
  `CMakeLists.txt`;
- LTO, sample-based PGO, AutoFDO, or BOLT;
- an assertion that every promotion will outperform its trainer.

Those exclusions keep the first experiment aligned with the repository's
Clang/LLVM toolchain while leaving room for later backends.

## Unit Of Profiling

The profile belongs to a loaded root-definition binary generation, not to an
instance UUID and not merely to a manifest id.

A root binary contains the code for its reachable IV module closure. Several
instances of one definition therefore exercise the same counters and should
contribute to one aggregate profile. The same imported module compiled into
two different root binaries has two independent profiles in the first design.

This matches the current build and binary ownership. Sharing dependency-level
profiles between roots would require a more stable compilation-unit identity
and is a separate optimization.

## Exact Build Identity

Profile data may only train a compatible binary. A future
`ModuleBuildIdentity` should include, directly or through a digest:

- the root path and complete reachable manifest/source closure;
- custom module CMake inputs and generated IV build glue;
- module descriptor ABI and relevant Intravenous headers/template support;
- compiler executable, compiler version, target triple, and target CPU
  policy;
- build configuration and effective compile/link options;
- the instrumentation scheme and its version.

The build identity is stricter than `definition_id`. A profile-use artifact is
also identified by the digest of the merged profile that produced it.

The current build workspace and signature are single-variant. Training,
ordinary, and profile-use builds will need isolated variant directories so
that CMake caches, object files, and output artifacts cannot overwrite or
silently reuse one another.

Profiles and promoted artifacts are derived build cache. They do not belong in
`iv_project.jsonl`. Persisting them across server restarts is useful only when
the exact build identity still matches.

## Lifecycle And Priority

For an eligible fresh identity:

1. Build an optimized Release generation with instrumentation enabled at both
   compile and link time.
2. Load and activate it through the ordinary definition and instance reload
   flow.
3. Count actual rendered blocks or samples for that generation. Wall-clock
   residency is insufficient because a paused, disconnected, or silent module
   may provide little representative training.
4. Once the training policy says the profile is useful, capture the raw
   profile at a quiescent task-runner boundary.
5. Perform raw-profile file I/O, `llvm-profdata merge`, and the profile-use
   build on the module build service, never on a task worker.
6. Load and prepare the candidate exactly like a source-triggered reload.
7. At a task-runner boundary, migrate state and activate the candidate.
8. Retain the training binary until both graph retirement and any final
   profile capture are complete.

A source or build-input change always outranks an optimization request. It
creates a new build identity, cancels or obsoletes work for the old identity,
and starts a new lifecycle. A late PGO result must carry its identity and be
discarded rather than replacing newer source.

If training overhead becomes unsafe, an eligible module must be able to fall
back to an ordinary Release generation. Failure and retry policy should use a
cooldown rather than rebuilding on every watcher poll.

## Profile Capture And Real-Time Safety

This is the main area that needs a technical spike before an architecture is
settled.

LLVM's profiling runtime supports multiple instrumented shared libraries and
can keep their raw profiles distinct with the `%m` filename modifier. Its
runtime also exposes buffer-oriented functions such as
`__llvm_profile_get_size_for_buffer()` and `__llvm_profile_write_buffer()`.
That suggests a generated, optional profiling-control export on each training
DSO could snapshot its own runtime without relying on a process-global symbol
lookup.

The preferred shape is:

- preallocate any snapshot storage away from the task-runner boundary;
- stop calls into that generation at an existing quiescent boundary;
- perform only a bounded in-memory snapshot at the boundary;
- hand the buffer to the build service for disk I/O and merging;
- keep the `ModuleRef` alive until the snapshot is complete.

The optional control export should be additive to descriptor ABI v2, rather
than changing v2 and making every existing module incompatible.

If an in-memory snapshot cannot be made bounded and allocation-free, use a
slower fallback: briefly replace the trainer with an already-built ordinary
generation, retire the trainer, capture it off-thread, and later promote the
profile-use generation. This costs an additional hot reload but avoids racing
live counters or doing filesystem work at a pass boundary.

Continuous file-backed profiling should not be the first approach. Platform
support and value-profiling behavior vary, and continuously dirtying mapped
profile pages is a poor default for a real-time DSP process.

## Concurrent Counter Updates

Different instances of one definition may execute concurrently on task-runner
workers while sharing the counters in their DSO.

Clang's default `-fprofile-update=single` counters can be inaccurate under
thread contention. `-fprofile-update=atomic` is accurate but adds overhead to
the hot path. The prototype must measure both on representative multi-instance
graphs before choosing a default. PGO tolerates approximate counts, but the
non-atomic option is acceptable only if profile merging remains valid and the
resulting optimization quality is repeatable.

Training overhead must be evaluated against underruns and tail block latency,
not just total throughput. The profiler is not useful if its temporary cost
breaks real-time execution.

## Suggested Ownership

PGO policy should not be embedded in `ModuleLoader` or
`IvModuleInstancesExecution`.

### `IvModuleOptimization` (future application module)

Owns:

- opt-in and eligibility policy;
- per-build-identity state (`training`, `profile-ready`, `optimizing`,
  `optimized`, `backoff`);
- training budgets and promotion cooldowns;
- raw and merged profile cache/retention;
- promotion requests and status diagnostics.

Consumes:

- definition generation activation/retirement events;
- rendered-sample or rendered-block progress;
- profile snapshot completion;
- variant build results.

### `IvModuleReload`

Continues to own build scheduling and reload results. It will eventually need
variant-aware build requests and must reject results whose build identity is
no longer current. Source-triggered and optimization-triggered work should be
serialized or coalesced by explicit priority, not by timing.

### `ModuleLoader`

Remains the build/load mechanism. It will eventually need a build request that
selects `ordinary`, `training`, or `profile-use`, isolated workspaces for each
variant, and an explicit matching `llvm-profdata` tool path. It should not
decide when a module has trained long enough.

### `IvModuleInstancesExecution`

Continues to own safe generation lifetime and instance execution. It may emit
cheap generation activity and retirement facts and participate in a
quiescent snapshot handshake. It must not run compilers, merge profiles, or
own profile cache policy.

## Custom CMake Contract

`iv_add_runtime_module()` is the natural target-level integration point for
instrumentation and profile-use compile/link options. Its generated export TU
and `SOURCES` can therefore participate automatically.

A custom `CMakeLists.txt` remains authoritative. Static or shared libraries it
builds separately will not automatically receive compatible instrumentation.
The first version should document that boundary and either:

- limit PGO to sources compiled directly into the runtime-module target; or
- expose an explicit IV PGO settings target that custom targets may link.

Silently producing a partial profile for arbitrary custom build graphs would
make performance results difficult to interpret.

## Required Invariants

- No profile file access, subprocess launch, profile merge, or compilation on
  a task-runner worker or activation boundary.
- A generation is never unloaded while executable graph state or profile
  capture can still call into it.
- Profile-use compilation never consumes data from a different build identity
  or incompatible LLVM toolchain.
- A source edit cannot be overwritten by a late optimization result.
- Failed training or promotion never removes the last working generation.
- Promotion uses the existing graph preparation, state migration, and
  revision-safe activation flow.
- Writes under the profile/build cache never look like module source changes
  to `DependencyWatcher`.
- PGO status is observable but does not create rapid notification or rebuild
  loops.
- Profile and generation retention is bounded.

## Diagnostics Worth Exposing

At minimum, logs and later UI/RPC projection should distinguish:

- PGO disabled or unsupported toolchain;
- training generation active and rendered-sample progress;
- profile capture/merge started or failed;
- profile mismatch or compiler warning;
- candidate build and load status;
- optimized generation active;
- obsolete candidate discarded because its identity changed;
- fallback/backoff and its reason.

These are optimization statuses, not ordinary source-build failures. A PGO
failure should not make a healthy module appear broken.

## Validation And Success Criteria

The first implementation should not start with orchestration. Start with one
Linux/Clang module and prove the binary/profile mechanics:

1. Build and load an instrumented module DSO.
2. Exercise several instances concurrently.
3. Snapshot or dump the correct DSO profile, retire it, and unload it without
   losing data or retaining unsafe callbacks.
4. Verify the raw profile with `llvm-profdata`, merge it, and complete a
   profile-use build without mismatch warnings.
5. Hot reload the optimized generation and verify state migration and DSP
   output correctness.
6. Repeat with a source edit during profile-use compilation and prove the
   stale result is discarded.

Performance validation should record:

- ordinary Release time per rendered block;
- instrumented Release overhead;
- profile-use Release time per block;
- p50, p95, and p99 block latency and audio underruns;
- single-instance and concurrent multi-instance behavior;
- profile merge/build latency, binary size, and cache growth.

The feature is successful only when a representative workload shows a
repeatable improvement larger than measurement noise, training does not cause
unacceptable real-time regressions, and the reload/lifetime invariants remain
intact.

## Open Questions

- Can the LLVM runtime's buffer snapshot be wrapped per DSO without symbol
  interposition surprises on Linux, macOS, and Windows?
- How long does a quiescent in-memory snapshot take for realistic IV module
  binaries, and can all storage be prepared beforehand?
- Is approximate non-atomic profiling good enough, or is atomic counter cost
  acceptable during a bounded training phase?
- Which activity measure best identifies representative training: rendered
  samples, active blocks, transport time, or a combination?
- Should the first training generation begin immediately, or should the user
  explicitly request training after an ordinary generation is known-good?
- How should a user retrain after their workload changes without editing
  source?
- Should persisted profiles be local to a project, global for global-module
  roots, or always ephemeral at first?
- What retention limit should apply to raw profiles, indexed profiles,
  ordinary/training/optimized workspaces, and copied generation libraries?
- How should custom CMake targets declare that all relevant code participates
  in the same PGO build?
- Is automatic rollback justified, and if so, what low-overhead runtime
  measurement can make that decision reliably?

## Later Directions

After a one-promotion implementation is proven:

- explicit retraining and profile reset;
- persisted compatible profiles across sessions;
- weighted merging of several representative training windows;
- periodic re-optimization with a strict cooldown and improvement gate;
- low-overhead sample-based profiling of already optimized generations;
- dependency-level profile reuse across root definitions;
- other compiler backends behind a toolchain-specific strategy.

## External References

- [Clang Compiler User's Manual: Profile Guided Optimization](https://clang.llvm.org/docs/UsersManual.html#profile-guided-optimization)
- [`llvm-profdata` command guide](https://llvm.org/docs/CommandGuide/llvm-profdata.html)
- [LLVM instrumentation profile format](https://llvm.org/docs/InstrProfileFormat.html)
- [LLVM profiling runtime interface](https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/profile/InstrProfiling.h)

