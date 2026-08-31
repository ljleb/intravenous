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
--stage execution-lowering # lower authored graph connections only
--stage execution-freeze # lower and freeze generated execution nodes only
--stage execution-connection-validation # validate generated connections only
--stage execution-connection-reflection # reflect generated connections only
--stage execution-reflection # lower and reflect execution nodes only
--stage execution-finalization-sorted # finalize through validation and sorting
--stage execution-finalization-scopes # additionally construct lowered scopes
--stage execution-finalization-subgraphs # additionally compile subgraphs
--stage execution-finalization-virtual-metadata # additionally build metadata
--stage execution-finalization-plan # additionally compile the SCC plan
--stage execution-finalization-dormancy # additionally compile dormancy groups
--stage execution-finalization-artifact-base # artifact preparation only
--stage execution-finalization-node-wrapper-scaffolding # bare wrappers only
--stage execution-finalization-node-wrappers # also construct node wrappers
--stage execution-finalization-artifact # build artifact without Graph freezing
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
`execution-lowering` stops immediately after generated-node and edge lowering.
Its delta from `authoring` identifies the graph-rewrite cost before any
connection validation, freezing, or reflection.
`execution-freeze` stops just before reflection's
`reflect_constant/substitute/extract` step, separating static-value freezing
from the C++26 reflection/template work.
`execution-connection-validation` stops before static-value construction. Its
delta from `authoring` isolates validation of generated connection mappings.
`execution-connection-reflection` isolates the generated `ConnectionNode`
subset, which is normally the dominant generated-node category in arithmetic
and routing-heavy graphs.
`execution-finalization-sorted` continues the execution-root preparation
through edge lowering, normalization, validation, and topological sorting, but
stops before lowered-scope construction, execution-plan and dormancy
compilation, and freezing the final `Graph` value. Its delta from
`execution-reflection` separates graph normalization from the post-sort
finalization pipeline.
`execution-finalization-scopes` additionally builds the lowered-scope
hierarchy. Its delta from `execution-finalization-sorted` isolates scope
membership and hierarchy construction.
`execution-finalization-subgraphs` additionally remaps lowered subgraph
descriptions to runtime node indices. Its delta from
`execution-finalization-scopes` isolates that nested-graph compilation pass.
`execution-finalization-virtual-metadata` additionally constructs the
execution-root virtual-node metadata. Its delta from
`execution-finalization-subgraphs` isolates that metadata pass.
`execution-finalization-plan` additionally builds the SCC execution plan. Its
delta from the mapping-only preceding work isolates SCC discovery and ordering.
`execution-finalization-dormancy` additionally builds dormant-subgraph groups.
Its delta from `execution-finalization-plan` isolates that pass.
`execution-finalization-artifact` additionally builds the graph artifact but
does not construct `Graph`; its delta from
`execution-finalization-dormancy` isolates artifact construction, while the
remaining delta to `execution-graph` is static-storage freezing.
`execution-finalization-artifact-base` stops before `GraphNodeWrapper` and
`GraphSccWrapper` construction. Its delta from dormancy isolates artifact
edge/buffer/latency preparation; the remaining delta to `artifact` isolates
the wrapper values used by static graph execution.
`execution-finalization-node-wrappers` additionally constructs each
`GraphNodeWrapper` but discards SCC wrappers. Its delta from `artifact-base`
isolates per-node static port and binding promotion; the remaining delta to
`artifact` isolates SCC-level wrapper construction.
`execution-finalization-node-wrapper-scaffolding` constructs the wrapper
objects without static ports, bindings, or IDs. Its delta from `artifact-base`
is wrapper bookkeeping; the remaining delta to `node-wrappers` is static data
promotion.

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

## Static versus generic graph-root A/B (2026-08-31)

This experiment replaces the generated module root
`StaticGraphRoot<iv_generated_module.graph>` with
`RuntimeGraphRoot {iv_generated_module.graph}`. The graph value remains
compile-time constructed and immutable, but the enclosing graph, SCC, and node
dispatch loops are no longer instantiated for every distinct graph value.

The results below are one GCC 16.2/Linux development-machine measurement of the
existing `projects/simple_sine/modules/saw` module. They are directional rather
than portable benchmarks, but both variants used the same source module, the
Release configuration, source introspection, PCH, and
`constexpr_cache_depth=0`.

### Module compilation

The static-root baseline and generic-root candidate were built in separate,
retained benchmark workspaces using:

```text
build-release/benchmark/iv_module_build_benchmark \
  --module projects/simple_sine/modules/saw --keep
```

| Metric | Static root | Generic root | Generic-root change |
| --- | ---: | ---: | ---: |
| Cold pipeline | 317.903 s | 61.007 s | 5.21x faster (-80.8%) |
| Cold generated-export compile | 305.510 s | 50.070 s | 6.10x faster (-83.6%) |
| Hot pipeline | 279.949 s | 51.056 s | 5.48x faster (-81.8%) |
| Hot generated-export compile | 279.522 s | 50.813 s | 5.50x faster (-81.8%) |

The PCH contribution was essentially unchanged (11.067 s static versus
9.809 s generic on cold builds); the reduction is in compiling and optimizing
the generated export translation unit.

The module-reload status timer is broader than the isolated benchmark: it
starts before `ModuleLoader` acquires the per-workspace build lock and stops
after the artifact is copied and loaded. In a later in-project reload of this
same generic candidate, the status reported 265.643 s, while that workspace's
Ninja log recorded 49.138 s for `root_export.cpp.o` and 0.222 s to link. The
remaining time occurred outside those Ninja edges (most plausibly serialized
workspace access); do not interpret a status duration as compiler time without
separate phase timings.

### Live execution

`iv_module_execution_benchmark` loads each retained module DSO, creates a
`BlockNodeExecutor`, warms it for 4,096 blocks, and then executes 131,072
64-sample blocks at 48 kHz. It measures batches of 256 blocks so timing calls
do not dominate individual DSP blocks.

Three alternating static/generic trials gave these mean timings:

| Metric | Static root | Generic root | Generic-root change |
| --- | ---: | ---: | ---: |
| Mean block time | 205.14 us | 193.94 us | 5.46% faster |
| Real-time factor | 6.50x | 6.87x | 5.7% higher |

The comparison was also pinned to one performance core (`taskset -c 0`) and
measured with `perf stat` over the same workload:

| Metric | Static root | Generic root | Generic-root change |
| --- | ---: | ---: | ---: |
| Mean block time | 218.31 us | 199.91 us | 8.43% faster |
| Task-clock | 29.498 s | 27.006 s | -8.45% |
| Cycles | 128.20 B | 117.96 B | -8.00% |
| Instructions | 525.24 B | 560.21 B | +6.66% |
| Branches | 95.54 B | 111.70 B | +16.92% |
| Branch misses | 144.19 M | 145.50 M | +0.91% |
| IPC | 4.10 | 4.75 | +15.9% |

The generic path executes more instructions and branches, but has higher IPC
and a lower branch-miss rate (0.130% versus 0.151%), yielding fewer total
cycles. On this workload the change is therefore a compile-time win without a
live-execution penalty.

`tests/intravenous_detach_regression_tests` verifies that the generic root
matches the static root's sample output and executes dormant groups correctly.
The generated candidate was also loaded and executed by the benchmark itself.
On Linux, `perf` is available in the repository dev shell for repeating the
counter measurement.
