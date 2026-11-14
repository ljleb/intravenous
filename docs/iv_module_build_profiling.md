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
measures the initial package compile and a source-only reload. The benchmark
still labels these phases `cold` and `hot`; those labels now describe package
workspace reuse, not PCH generation. The shared DSL PCH is an application
build product and is already present for both phases. The workspace is
retained and printed at the end, so the generated CMake project, Ninja log,
and timing sidecar can be inspected.

To use another module or workspace:

```text
scripts/profile_module_reload.sh \
  --module projects/simple_sine/modules/saw \
  --workspace /tmp/iv-module-reload-profile
```

The wrapper accepts `--skip-build`. Set `IV_BUILD_DIR` or `IV_BUILD_JOBS` when
the release build directory or parallelism differs from the defaults. Package
LLVM is intentionally fixed at O0; optimization is no longer a benchmark
option.

## Report fields

The first line for each `cold` and `hot` phase reports:

- `pipeline_ms`: complete `ModuleLoader::compile_package` wall time.
- `configure_us` and `ninja_build_us`: the two external build invocations.
- `package_finalize_ms`: the newly appended Ninja edge duration for producing
  the finalized `.ivpkg.bc`. The shared DSL PCH is built by the application,
  outside an individual package build, and therefore is not a package-Ninja
  timing field.

The second summary line, `iv-module-finalizer-profile`, exposes LLVM pre-link
optimization, node-code reduction, node-code optimization, and machine-code
generation. These are parts of `finalizer_ms`, not additional time. They can
dominate finalization even when bitcode parsing and metadata processing are fast.

The following lines, prefixed `finalizer_`, come from
`cmake-build/iv-package-finalizer-timings.txt` in that workspace. They split
the package finalizer into bitcode parse/link, package-metadata validation,
package-metadata injection, finalized-bitcode write, and total time. Values
are integer microseconds.

The build benchmark deliberately does not ORC-load the finalized package
bitcode, keeping compile measurements comparable. When a caller gives
`ModuleLoader` a `LogSink`, it also receives `package-orc-load` and
`graph-configuration` timings for the load/configuration half of a real
reload.

`ninja_log_delta=1` means `package_finalize_ms` describes only the new build
edge. If Ninja rewrites or compacts `.ninja_log`, the benchmark prints
`ninja_log_delta=0` and leaves that field at zero rather than making a false
attribution.

## How to use the result

Run the focused command twice before drawing a conclusion, comparing the
`hot` lines on the same machine and checkout. The next change follows the
largest measured stage:

- high `configure_us`: split configuration invalidation from source rebuilds;
- high `ninja_build_us`: inspect the Clang time trace and package source work;
- high `finalizer_bitcode_parse_link_us`: reduce package-facing LLVM inputs;
- high package-metadata finalizer timings: reduce metadata volume or validation
  work;
- high load/configuration timings: profile ORC loading and graph configuration
  separately.

The broader `scripts/verify_release_performance.sh` is for release validation,
not the quick investigation loop: it builds the full release tree, runs all
release tests, and also runs the execution benchmark.

## Simple-sine measurements, 2026-11-14

The owner ran `scripts/profile_module_reload.sh --simple-sine-modules`.
Detailed logs were retained in `/tmp/intravenous-module-reload-profile.logs`,
with Clang traces under `/tmp/intravenous-module-reload-profile`.

The six hot package rebuilds total 22.712 seconds. Their combined times are:

| Work | Seconds |
| --- | ---: |
| CMake configuration | 0.255 |
| Compilation and remaining Ninja overhead, estimated by subtraction | 15.623 |
| Finalization, measured from Ninja edges | 6.832 |

Finalization includes 3.465 seconds of pre-link LLVM optimization, 1.602 seconds
of node-code optimization, and 1.330 seconds of machine-code generation.
These three stages were present in detailed logs but absent from the original
wrapper summary. The wrapper now prints them on a second line.

The accompanying application log reports 23.172 seconds for its package batch.
Its diagnostic log shows six source compilations and finalizations, with CMake
reusing existing workspaces. `ModuleLoader::Impl::load_packages` compiles its
candidates sequentially. The summed package measurements therefore explain
nearly all of that observed delay. They are separate runs, not a per-stage
measurement of the application run. The remaining difference cannot be assigned
precisely from these logs.

Clang traces put most translation-unit time in the frontend. For example, the
hot `saw` compile spends 2.113 of its 2.209 seconds there. Template instantiation
includes `make_node_build_request` for common nodes such as `Constant`, `Sum`,
and arithmetic nodes. The timings overlap and must not be added. This is work
that remains after loading the shared PCH, not evidence of PCH regeneration.

The application currently passes an empty `ModuleLoader::LogSink` from
`PackageJit::ensure_loader`. Its ordinary package-status messages therefore do
not expose the loader's optional ORC/configuration timings. Knob compilation
uses `ProjectGraph::control_compile_worker`, not the package-status path.
Ordinary code-only knob compilations produce no rebuild status messages.
`IV_EDIT_TIMING` currently reports structural rebuilds and storage-changing
control successors, not every code-only control compilation.
