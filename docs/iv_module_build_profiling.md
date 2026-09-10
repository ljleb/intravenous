# IV module reload profiling

This is the current measurement workflow for the Clang/LLVM module-reload
pipeline. Historical GCC-era measurements were removed from this document;
Git retains them if they are ever useful for archaeology.

## Focused warm-reload measurement

From the repository root, run:

```text
scripts/profile_module_reload.sh
```

It builds only `iv_module_build_benchmark`, snapshots
`projects/simple_sine/modules/saw` into a managed temporary workspace, then
measures a cold build and a source-only hot reload. The workspace is retained
and printed at the end, so the generated CMake project, Ninja log, and timing
sidecar can be inspected.

To use another module or workspace:

```text
scripts/profile_module_reload.sh \
  --module projects/simple_sine/modules/saw \
  --workspace /tmp/iv-module-reload-profile
```

The wrapper accepts `--optimization O0|O3` and `--skip-build`. Set
`IV_BUILD_DIR` or `IV_BUILD_JOBS` when the release build directory or
parallelism differs from the defaults.

## Report fields

The first line for each `cold` and `hot` phase reports:

- `pipeline_ms`: complete `ModuleLoader::compile_source` wall time.
- `configure_us` and `ninja_build_us`: the two external build invocations.
- `pch_ms`, `export_ms`, and `link_ms`: newly appended Ninja edge durations.
- `generation_copy_us`: copying the finished DSO into its unique generation
  directory.

The following lines, prefixed `finalizer_`, come from
`cmake-build/iv-module-finalizer-timings.txt` in that workspace. They split
the LLVM finalizer into bitcode parse/link, node-record scanning, metadata
load/bind, authoring-module clone, JIT creation/materialization,
builder-session setup, registered-source-module authoring, JIT release, graph serialization,
module-data injection, runtime optimization, native object emission, native
link, and total time. Values are integer microseconds.

The build benchmark deliberately does not load the DSO, keeping compile
measurements comparable. When a caller gives `ModuleLoader` a `LogSink`, it
also receives `dynamic-library-load` and `runtime-graph-materialization`
timings for the load half of a real reload.

`ninja_log_delta=1` means the PCH/export/link values describe only the new
build edges. If Ninja rewrites or compacts `.ninja_log`, the benchmark prints
`ninja_log_delta=0` and leaves those three fields at zero rather than making a
false attribution.

## How to use the result

Run the focused command twice before drawing a conclusion, comparing the
`hot` lines on the same machine and checkout. The next change follows the
largest measured stage:

- high `export_ms`: reduce module-facing headers or generated IR;
- high `configure_us`: split configuration invalidation from source rebuilds;
- high `finalizer_runtime_optimize_us` or
  `finalizer_native_object_emit_us`: evaluate O2 versus O3 before designing
  caching;
- high JIT/authoring timings: reduce builder-side IR and work;
- high load timings: profile DSO and runtime graph materialization separately.

The broader `scripts/verify_release_performance.sh` is for release validation,
not the quick investigation loop: it builds the full release tree, runs all
release tests, and also runs the execution benchmark.
