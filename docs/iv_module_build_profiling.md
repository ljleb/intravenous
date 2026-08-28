# IV module build profiling

## Reproducible benchmark

Build and run `iv_module_build_benchmark` from the repository root:

```text
cmake --build build-release --target iv_module_build_benchmark
build-release/benchmark/iv_module_build_benchmark --voices 1 --keep
```

It generates an isolated project under the system temporary directory, then
compiles a cold artifact and a source-only hot reload. It deliberately does not
load the shared library, so every compile stage is measured in the same way.
Each report includes the end-to-end pipeline time plus the individual Ninja
times for the PCH, generated export translation unit, and link edge. `--keep`
prints the workspace path for inspection; otherwise the benchmark removes its
workspace. A retained workspace is only cleared by a later benchmark invocation
when it contains the benchmark's marker file.

`ninja_log_delta=1` means the per-edge numbers came from newly appended Ninja
records. If Ninja rewrites or compacts its log, the benchmark prints
`ninja_log_delta=0` and deliberately leaves those per-edge values at zero
rather than misattribute cold work to the hot reload.

The default is a full one-oscillator module, which is the fast, repeatable
baseline. Increase `--voices` to scale the authored graph while keeping the
module shape fixed. The generated project is independent of the example and
working projects, so edits to those projects cannot perturb the source being
measured.

To benchmark an existing project module without touching its source tree or
build cache, pass its directory (or `iv_module.json`) with `--module`:

```text
build-release/benchmark/iv_module_build_benchmark \
  --module projects/simple_sine/modules/saw --gcc-time-report --keep
```

The benchmark snapshots the module's enclosing project into its managed
temporary workspace, omitting `build`, `out`, and `.git`. Its hot-reload marker
is written only to that copy. Existing-module snapshots currently require the
usual `module.cpp` entry point.

## Separating compile-time stages

Use `--stage` to compile progressively more of the generated translation unit:

```text
--stage parse       # include and semantically compile the authored source
--stage authoring   # also execute module_main against GraphBuilder
--stage metadata    # build and freeze introspection metadata only
--stage execution-reflection # lower and reflect execution nodes only
--stage execution-graph # finalize and freeze Graph, without static tick code
--stage execution   # finalize and instantiate the static execution graph only
--stage full        # production execution graph plus metadata (the default)
```

The stages are diagnostic artifacts; only `full` has the normal loadable module
ABI. They preserve the configured optimization level. In particular,
`execution` and `full` still instantiate the same statically expanded graph at
the build's existing `-O3` optimization level. `execution-graph` performs the
same graph finalization but deliberately stops before
`StaticGraphRoot<GraphValue>` is instantiated; its delta to `execution` is the
cost of emitting and optimizing the static tick path.

`execution-reflection` stops before edge lowering and final graph algorithms.
Its delta from `authoring` identifies how much of the execution-root build is
spent turning generated nodes into value-specialized reflected operations.

`--source-shape empty|input|nodes|connected|full` progressively changes the
body of the generated `module_main`. This provides a second axis for isolating
front-end costs without editing a real module. `--voices` controls the number of
nodes for the shapes that contain oscillators.

Two additional A/B controls isolate build infrastructure:

```text
--no-source-introspection # omit the GCC source annotation plugin
--no-pch                  # compile the same TU without the module PCH
```

GCC's constexpr memoization policy can be swept without changing the generated
module or its optimization level:

```text
--constexpr-cache-depth 4
--constexpr-cache-depth 8
--constexpr-cache-depth 16
--constexpr-cache-depth 32
```

This maps directly to GCC's `-fconstexpr-cache-depth` option. Leaving it
unset uses GCC's default. Compare full-stage hot runs on the same snapshot and
record both wall time and GGC memory; larger caches can trade memory for less
constant-evaluation time.

Both are diagnostics only; source introspection and the PCH remain enabled by
default. The no-PCH path is also useful as a header self-containment check: the
generated export and public authoring headers must compile without relying on
declarations accidentally supplied by the PCH.

Add `--gcc-time-report` to keep a `compiler.time.log` beside the generated
module workspace (and retain that workspace). It enables GCC's `-ftime-report`
and `-ftime-report-details`. The benchmark prints a `gcc_hot` summary containing
the hot export's total, constant-expression, template-instantiation, deferred
front-end, and optimize/codegen wall times, plus GCC's total GGC allocation.
The complete report remains available at the printed path.

GCC does not provide elapsed compile time per C++ function. The stage and source
shape axes are therefore the reliable subsystem-level profile. Optimization
diagnostics can identify which functions were transformed, but do not attribute
elapsed time to them.

For example, on the GCC 16.2 development machine used while adding the stage
benchmark, the one-voice hot export produced these representative results:

| Stage | Hot export | GCC constexpr | GCC optimize/codegen |
| --- | ---: | ---: | ---: |
| parse | 3.15 s | 0.14 s | 0.11 s |
| authoring | 3.20 s | 0.24 s | 0.11 s |
| metadata | 3.62 s | 0.51 s | 0.10 s |
| execution | 9.73 s | 0.89 s | 4.44 s |
| full | 9.13 s | 0.98 s | 4.18 s |

These numbers are not portable benchmarks; compare runs on the same machine and
checkout. The useful result is the shape of the deltas: metadata construction is
small for this graph, while the execution/full delta is dominated by preserving
the statically specialized `-O3` execution code.

The stage split also caught an eager graph-compiler instantiation regression:
the full one-voice hot export had taken about 27.3 s, including 16.45 s of
constant-expression evaluation and roughly 4.1 GiB of GGC allocation. Selecting
only the requested generated `Broadcast<Arity>` specialization reduced the same
measurements to 9.13 s, 0.98 s, and 738 MiB respectively. Keep the parse-stage
baseline in performance investigations: graph algorithms should not impose
large constant-evaluation costs merely because their headers were included.

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

The PCH is performance-positive rather than merely present. In the same
one-voice full-stage measurement, disabling it increased the hot export from
about 9.1 s to 12.6 s. Always compare `pch_ms=0` on the hot line before drawing
conclusions from an individual run.

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
