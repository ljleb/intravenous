# IV module build profiling

## Reproducible benchmark

Build and run `iv_module_build_benchmark` from the repository root:

```text
cmake --build build-release --target iv_module_build_benchmark
build-release/benchmark/iv_module_build_benchmark --voices 1 --keep
```

It generates an isolated project under the system temporary directory, then
reports a cold load and a source-only hot reload. Each report includes the
end-to-end pipeline time plus the individual Ninja times for the PCH, generated
export translation unit, and link edge. `--keep` prints the workspace path for
inspection; otherwise the benchmark removes its workspace after unloading the
module. A retained workspace is only cleared by a later benchmark invocation
when it contains the benchmark's marker file.

`ninja_log_delta=1` means the per-edge numbers came from newly appended Ninja
records. If Ninja rewrites or compacts its log, the benchmark prints
`ninja_log_delta=0` and deliberately leaves those per-edge values at zero
rather than misattribute cold work to the hot reload.

The default is one oscillator, which is the fast, repeatable baseline. Increase
`--voices` to scale the authored graph while keeping the module shape fixed.

Add `--gcc-time-report` to keep a `compiler.time.log` beside the generated
module workspace (and retain that workspace). It enables GCC's `-ftime-report` and
`-ftime-report-details`, which report compiler phases and subphases (including
constant-expression evaluation, template instantiation, and optimization).
GCC does not provide per-C++-function compile-time timings; the report is the
most granular timing source it exposes. Optimization diagnostics can identify
functions, but do not attribute elapsed time to them.

## Inspecting an existing module

An iv module keeps one CMake/Ninja workspace per root module. For a project
module, find it under:

```text
<project-root>/build/iv/build/<module-id>_<path-hash>/<Debug|Release>/cmake-build
```

The workspace's `compile_commands.json` is the authoritative record of the
commands used for the generated export translation unit. With PCH enabled it
contains both:

```text
-include .../CMakeFiles/iv_runtime_module.dir/cmake_pch.hxx
-Winvalid-pch
```

and the corresponding `cmake_pch.hxx.gch` exists below the same CMake build
directory. A source-only reload should rebuild `root_export.cpp.o` without
rebuilding that `.gch` file.

Ninja records the elapsed time of every build edge in `.ninja_log`. It is the
quickest way to separate the cold PCH cost from the hot-reload cost:

```text
... cmake_pch.hxx.gch           # cold PCH construction
... generated/root_export.cpp.o # compile-time graph construction and codegen
```

For an ad-hoc GCC phase report, configure the retained workspace with
`-DCMAKE_CXX_FLAGS='-ftime-report -ftime-report-details'`, then rebuild the
module target. GCC writes the report to stderr. This does not create a Chrome
trace.
