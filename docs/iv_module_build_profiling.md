# IV module build profiling

> **Status:** This is a profiling notebook, not a statement of current
> performance. Every numeric result is specific to the recorded checkout,
> machine, compiler, module shape, and command line; it can become stale as
> soon as graph construction changes. Treat the commands and measurement
> methodology as durable, but rerun them on the current checkout before using
> any number for a decision. Dated A/B sections preserve their historical
> context and should not be read as current baselines.

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

### Rejected static-promotion microexperiment (2026-08-31)

Batching the frozen `ConnectionNode` objects into one static array before
reflection did not reduce the important work: each object still independently
froze its nested spans and strings. The retained `saw` hot build measured
50.917 s for the generated export (49.950 s GCC total, 8,024M reported GGC,
and 40.340 s constexpr), versus recent generic-root runs around 49–51 s for the
same edge. It also increased GGC allocation and constexpr time, so the change
was discarded. A useful arena experiment must flatten the nested tables, not
only batch their owning objects.

### Rejected offset-slice connection arena (2026-08-31)

A fuller arena flattened the connection-node strings and nested tables. GCC
cannot reflect an interior pointer such as `storage.data() + offset`, so the
experiment changed the static views to base-pointer, offset, and size triples.
It also needed 16-node shards to avoid an optimizer ICE. That workaround made
the retained `saw` export slower (51.560 s total; 41.120 s constexpr; 8,228M
reported GGC), produced a 51.2 MiB module instead of the generic-root
baseline's 4.12 MiB, and was 1.1% slower in execution (196.219 us/block versus
194.135 us/block). The implementation was reverted.

### Lowering metadata and sparse-index pruning (2026-08-31)

The lowering path was changed to avoid reconstructing virtual-port metadata
from every reflected runtime node when the authored virtual-node record already
owns that metadata. It also avoids maintaining ordinary-edge source indexes
unless detach or subgraph traversal needs them, and uses a dense bitmap for
authored sample-input binding state.

On the same retained `saw` workload, one hot build measured 46.199 s for the
generated export (45.720 s GCC total, 7,134M reported GGC, and 36.340 s
constexpr). A single execution run measured 192.596 us/block. These are
historical single-run observations, not current baselines or a runtime A/B;
rerun the documented commands on the current checkout before making a
performance decision.

### Lookup-only constexpr hash indexes (2026-08-31)

The lowering connectivity indexes and sample-group lookup were changed from
ordered flat containers to small open-addressing constexpr hash containers.
Those relations require only insertion and membership/lookup, so their
iteration order is not semantically observed; graph structures whose ordering
is meaningful remain flat ordered containers.

After all 419 tests passed, one hot `saw` build measured 43.944 s for the
generated export (43.500 s GCC total, 6,716M reported GGC, and 34.380 s
constexpr). Against the immediately preceding pruning observation, that is a
2.255 s (4.9%) reduction in generated-export time. One execution run measured
193.223 us/block, versus 192.596 us in the preceding single run; this small
difference is not a controlled runtime comparison. These are historical
single-run observations, not current baselines; rerun the documented commands
on the current checkout before making a performance decision.

### Finalization adjacency pruning (2026-08-31)

Dangling-output stubbing now records only the two output-membership relations
it actually queries, rather than constructing four source/target maps. The
event hyperedge pass likewise uses lookup-only reverse adjacency maps, while
the canonical graph edge sets remain ordered.

After all 419 tests passed, one hot `saw` build measured 39.012 s for the
generated export (38.590 s GCC total, 5,842M reported GGC, and 29.500 s
constexpr). That is 4.932 s (11.2%) below the immediately preceding hash-index
observation. One execution run measured 191.903 us/block, but this is not a
controlled runtime comparison. These are historical single-run observations,
not current baselines; rerun the documented commands on the current checkout
before making a performance decision.

### Batched edge remapping during node permutation (2026-08-31)

Node permutation remaps every sample and event edge. Instead of individually
inserting each remapped edge into a flat set, the compiler now collects edges,
sorts and deduplicates once, then constructs the canonical sorted sets.

After all 419 tests passed, one hot `saw` build measured 36.763 s for the
generated export (36.330 s GCC total, 5,378M reported GGC, and 27.450 s
constexpr). That is 0.531 s (1.4%) below the immediately preceding dense-table
observation. One execution run measured 190.246 us/block; it is not a
controlled runtime comparison. These are historical single-run observations,
not current baselines; rerun the documented commands on the current checkout
before making a performance decision.

### Dense compiler input-latency tables (2026-08-31)

Two whole-graph compiler passes previously propagated latency through sorted
maps keyed by `(node, input port)`. Those keys occupy a dense domain, so they
now use one compact offset table for real-node input ports and public-output
ports. This does not affect generated runtime code.

After all 419 tests passed, one hot `saw` build measured 37.294 s for the
generated export (36.870 s GCC total, 5,482M reported GGC, and 27.570 s
constexpr). That is 1.718 s (4.4%) below the immediately preceding stable
39.012 s observation. One execution run measured 192.635 us/block; it is not
a controlled runtime comparison. These are historical single-run observations,
not current baselines; rerun the documented commands on the current checkout
before making a performance decision.

### Dense compiler sample-source slots (2026-08-31)

The compiler's completed sample-source relation has one source per input and a
dense node-input domain. It now stores optional source slots by input ordinal,
with a dynamic public-output extension, instead of a flat map.

After all 419 tests passed, one hot `saw` build measured 36.602 s for the
generated export (36.200 s GCC total, 5,251M reported GGC, and 27.120 s
constexpr). This is only 0.161 s below the prior 36.763 s single observation,
so it is not independently conclusive. One execution run measured 193.845
us/block; it is not a controlled runtime comparison.

### Direct singleton execution plans (2026-08-31)

After topological permutation, no-detach graphs with strictly forward
dependencies have only singleton SCCs. The compiler now recognizes that case
and directly emits its regions and execution order, bypassing Tarjan, the
in-region ordering pass, and SCC-DAG construction. Dormancy planning also
returns immediately when there are no lowered subgraphs.

After all 419 tests passed, one hot `saw` build measured 35.302 s for the
generated export (34.920 s GCC total, 5,144M reported GGC, and 26.070 s
constexpr). That is 1.300 s (3.6%) below the immediately preceding 36.602 s
observation. One execution run measured 190.956 us/block; it is not a
controlled runtime comparison. These are historical single-run observations,
not current baselines; rerun the documented commands on the current checkout
before making a performance decision.

### Cumulative pipeline-stage profile (2026-08-31)

The generated export originally placed authoring, lowering, compilation, and
static-metadata promotion in one consteval initializer. The benchmark now has
diagnostic cumulative stages which return a scalar only after completing the
selected work. This preserves the constexpr work while avoiding an invalid
attempt to retain dynamically allocated intermediate IR as a constexpr object.

After the release test gate completed, one `saw` run measured:

| Cumulative stage | Hot export | GCC total | Constexpr | GGC |
| --- | ---: | ---: | ---: | ---: |
| Authoring | 9.240 s | 9.170 s | 5.840 s | 1,492M |
| Lowering | 23.136 s | 22.980 s | 19.260 s | 3,885M |
| Compilation | 31.315 s | 31.120 s | 26.100 s | 4,898M |
| Static metadata | 31.190 s | 30.990 s | 25.810 s | 4,926M |
| Full production export | 35.351 s | 34.990 s | 26.030 s | 5,134M |

The useful deltas are approximately 5.840 s for authoring, 13.420 s for
lowering, and 6.840 s for compilation. Static metadata is within single-run
noise here. The remaining roughly 3.8 s in `full` is its reported optimized
code generation and production wrapper emission, not more constexpr work.
Accordingly, the next detailed profile should subdivide `GraphLowerer::lower`,
rather than add path-specific compiler shortcuts. The execution handoff did
not run in this attempt because of a script typo, so this observation has no
new runtime sample.

### GCC source-level constexpr attribution (2026-09-01)

The hot `saw` generated-export translation unit was first recorded with the
configured Nix GCC 16.2 compiler (35,568 `perf` samples). Its dominant work was
the constexpr evaluator: `cxx_eval_constant_expression` had 14.84% exclusive
samples and `cxx_eval_call_expression` 5.76%. Allocation/collection, stores,
function-definition lookup, and constexpr-cache work were also material, while
final optimization/code generation was not the dominant cost.

For line and callsite attribution, a separate GCC 16.2 checkout was built with
debug information and an opt-in inclusive/self-time profiler around
`cxx_eval_call_expression`. The exact generated source, target options,
source-introspection plugin, and a PCH compiled by that GCC were used. A
P-core-only recording collected 34,763 samples over 34.83 seconds. This is a
source-attribution run, not a replacement performance baseline: the GCC
frontend itself was built with `-O2 -g`, whereas the configured Nix compiler is
the release baseline.

The profiler attributes the current lowering work as follows:

| constexpr function/callsite | Inclusive evaluator CPU time | Calls | Observation |
| --- | ---: | ---: | --- |
| `GraphCompiler::compile` | 23.246 s | 1 | Contains the full authoring/lowering/compilation flow. |
| `GraphLowerer::lower` | 15.263 s | 2 | Lowering remains the largest named pipeline phase. |
| `lower_runtime_output_observers` | 4.335 s | 1 | Materializes every virtual runtime output member. |
| `materialize_sample_output_channels` | 4.187 s | 35 | 4.166 s is reached directly from the observer loop. |
| `sample_output_port_for_channels` | 5.525 s | 59 | Repeated global logical-output rediscovery. |
| `NodeBundle::sample_output_descriptor` | 2.845 s | 19,038 | Returns copied `OutputConfig` descriptors through a variant visit. |
| `OutputConfig` construction | 1.890 s | 24,452 | Configuration copying is independently visible. |

`sample_output_port_for_channels` scans every bundle and every output port.
For each candidate it resolves a descriptor by value, constructs a temporary
`std::vector` of channel IDs, and compares it with the already-known semantic
channels. The call at `materialize_sample_output_channels` therefore performs a
whole-graph identity lookup even though its channel IDs already encode the
candidate bundle and port. This confirms the earlier lowering-complexity report
on the current full production export rather than identifying a new GCC
frontend bottleneck.

The first follow-up should replace that rediscovery with an identity-preserving
check: derive the first `{bundle, port}` from the semantic channels, verify the
remaining channels refer to that same output in order, then use it directly.
The descriptor/channel helpers should subsequently expose references or compact
layout metadata in the lookup path, avoiding `OutputConfig` and temporary
channel-vector construction. Re-profile before attempting further static
promotion changes; the latter is visible, but is not the first lowering
culprit.
