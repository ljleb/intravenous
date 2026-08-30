# IV module compile-time profiling record

## Scope and reproducibility

This is a builder-side profiling-lab record, not an optimization proposal and
not a comparison between unrelated source revisions.

The fixed workload is the retained 17-voice `simple_sine` saw module:

```text
/tmp/iv-bench-simple-sine-saw-full-cached-virtual-ports/project/modules/saw
```

All results use:

```text
build-release/benchmark/iv_module_build_benchmark \
  --module /tmp/iv-bench-simple-sine-saw-full-cached-virtual-ports/project/modules/saw \
  --stage <cut-point> --workspace /tmp/<unique-workspace> --keep \
  --gcc-time-report
```

The comparable values are the hot-phase GCC `-ftime-report` entries. All
listed runs used `pch=1`, `source_introspection=1`, and the benchmark-reported
`constexpr_cache_depth=0`. Hot runs report `pch_ms=0`, so PCH is being reused;
it is not the cause of the measured hot cost. `ggc` is GCC's reported `M`
value, not peak RSS.

The diagnostic stages stop at named construction cut points and emit a marker
or profile. They do not instantiate the final static execution root, so they
measure builder constant evaluation without changing the optimized runtime
representation.

## Workload shape

`--stage metadata-profiler` reported:

| Measure | Value |
|---|---:|
| authored bundles | 403 |
| authored concrete / tiled / subgraph bundles | 336 / 66 / 0 |
| authored sample connections / endpoints | 289 / 825 |
| authored event connections / endpoints | 0 / 0 |
| authored virtual nodes / members / port mappings | 3 / 35 / 9 |
| authored public ports | 3 |
| lowered nodes | 410 |
| lowered generated / `ConnectionNode` / runtime nodes | 74 / 71 / 3 |
| lowered sample input / output ports / edges | 543 / 407 / 544 |
| maximum sample fan-in / fan-out | 1 / 18 |
| metadata ports / source infos | 950 / 86 |
| lowered scopes | 0 |

The graph is sparse, has no event edges or nested scopes, and has maximum
fan-in one. Its 17 voices do expand into many bundles, but neither dense
routing nor recursion explains the observed compile cost.

## Coarse cumulative cut points

These same-snapshot measurements established where to investigate:

| Cut point | GCC total | constexpr evaluation | GGC |
|---|---:|---:|---:|
| authoring | 9.55 s | 6.09 s | 1,471 M |
| execution projection | 11.57 s | 7.90 s | 1,782 M |
| execution sample lowering | 33.91 s | 30.36 s | 5,854 M |
| execution lowering | 38.51 s | 34.82 s | 6,769 M |
| metadata profiler | 39.93 s | 36.25 s | 7,110 M |

Projection to sample lowering adds about **22.5 s of constant evaluation** and
**4,072 M of GGC**. Later lowering and metadata work are material but are not
the first bottleneck to remove.

## Detailed `lower_samples()` attribution

The normal sequence was factored only to expose existing operations:

```text
project_bundles
sample_groups
lower_connected_sample_groups
lower_vacant_sample_inputs
lower_subgraph_sample_bindings
```

No normal execution behavior was changed by these cut points. The detailed
values are hot measurements; sub-second differences between independently run
GCC reports are noise, not wins.

| Cumulative cut point | Marker | constexpr evaluation | GGC | Increment from prior constexpr cut point |
|---|---:|---:|---:|---:|
| projection | 336 lowered nodes | 7.70 s | 1,792 M | — |
| sample grouping | 273 groups | 8.56 s | 1,880 M | +0.86 s |
| raw group/connection/channel iteration only | checksum 206,346 | 8.41 s | 1,885 M | within noise of grouping |
| connected sample lowering | 407 lowered nodes | 28.64 s | 5,564 M | **+20.08 s** |
| connected lowering plus vacant inputs | 407 lowered nodes | 29.70 s | 5,874 M | +1.06 s |
| full sample lowering | — | 29.73 s | 5,862 M | +0.03 s |

There are no subgraphs in this workload, so the final subgraph-binding pass
has no measurable tail. Grouping and iterating the 289 connections and raw
channels is cheap; the dominant work begins inside
`lower_connected_sample_groups()`.

### Destructive connected-lowering probes

These probes preserve projection, grouping, and connected-group control flow,
but return at progressively earlier points in `lower_connection_node()`. They
are diagnostic-only and **not additive**: source-only and target-only cannot
be summed.

| Diagnostic endpoint | constexpr evaluation | GGC | Meaning |
|---|---:|---:|---|
| source resolution only | 27.27 s | 5,365 M | source channels resolved; no node made |
| target resolution only | 27.26 s | 5,380 M | target channels resolved; no node made |
| source + target resolution + conversion plan | 27.72 s | 5,384 M | no `ConnectionNode` materialization |
| materialized node spec, not appended | 28.81 s | 5,504 M | spec construction adds at most about 1.1 s here |
| appended node, no edges | 28.39 s | 5,558 M | appending/retaining is below this probe's noise scale |
| normal connected sample lowering | 28.64 s | 5,564 M | full connected path |

This rules out creating, appending, and wiring the 71 generated
`ConnectionNode`s as the explanation for the roughly 20-second connected
lowering increment. It does not prove that source and target resolvers each
independently cost 19 seconds: both probes execute shared group policy first.

## Measured dominant operation: global direct-source lookup

Static inspection identified a shared expensive policy path:

```cpp
direct_sample_source(type, channels)
  -> bundles.sample_output_port_for_channels(type, channels)
```

`sample_output_port_for_channels()` scans all node bundles and sample output
ports until it rediscovers the logical port already described by the supplied
channels. For every matching-type candidate it:

1. resolves an `OutputConfig` by value through `NodeBundle`'s variant visitor;
2. creates a `std::vector<SampleOutputChannelId>`; and
3. compares that vector with the requested channel span.

The exact accounting profile for real connected-group control flow is:

| Operation | Count |
|---|---:|
| source-channel resolutions | 406 |
| target-channel resolutions | 315 |
| channel conversion plans | 87 |
| `direct_sample_source` global lookups | 191 |
| bundles visited by those lookups | 28,173 |
| output ports visited | 28,347 |
| matching-type candidate ports / channel vectors constructed | 23,867 / 23,867 |
| channel-element comparisons | 23,867 |
| successful lookups | 191 |

On average, each real lookup visits **147.5 bundles**, **148.4 output ports**,
and constructs **125 temporary channel vectors**. All 191 lookups eventually
succeed. This is the relevant scaling mechanism, rather than the raw number
of authored connections alone.

A deliberately non-semantic control performs this global lookup once for each
of the 289 authored connections, after projection and grouping but without
normal connection-node construction:

| Diagnostic | GCC total | constexpr evaluation | GGC |
|---|---:|---:|---:|
| 289 direct-source global lookups | 36.29 s | 32.56 s | 6,171 M |

The control has more lookups than the normal path (289 versus 191), so it
cannot be subtracted from the full path. It is strong causal evidence: global
lookup alone reproduces and exceeds the connected-lowering constexpr cost,
whereas raw traversal does not.

## Supported conclusions

- PCH is implemented and reused on hot builds in this setup (`pch=1`, hot
  `pch_ms=0`).
- `lower_connected_sample_groups()` is the first builder hotspot: about 20 s
  and 3,684 M reported GGC beyond grouping on this frozen graph.
- The cost is not primarily `ConnectionNode` construction, appending, or edge
  insertion.
- Direct-source identity lookup has accidental global-search complexity. The
  input already contains `SampleOutputChannelId {bundle, port, channel}` but
  the path scans to rediscover that identity.

Not yet supported:

- an exact percentage of connected-lowering time caused by the lookup; other
  resolution and validation share the same policy path;
- a claim that reflected node value specialization is the dominant *builder*
  cost; that work occurs later than these cut points;
- any runtime-performance conclusion. The static Graph/SCC/node execution
  representation was not replaced or measured here.

## Main-branch candidate enabled by this record

The first candidate should replace the global
`sample_output_port_for_channels()` search on this fast path with an
identity-preserving local check:

```text
first {bundle, port} + validate same logical port/channel sequence
    -> NodeBundlePortId
```

That changes the lookup from a bundle/port scan with temporary vectors to work
proportional only to the supplied channel span. It needs focused mono and tiled
direct-routing regression tests, then a same-workload
`execution-connected-sample-lowering` measurement before any full-module or
DSP comparison. The static execution representation remains an invariant.

## Other data and caveats

An earlier full-module measurement from a different source revision reported
208.49 s GCC total, 113.79 s constexpr evaluation, 27,860 M GGC, and 87.69 s
optimization/code generation. It demonstrates that later finalization and
static code generation remain important, but is not comparable to the
same-snapshot builder tables above.

`build_virtual_metadata()` currently caches connected input/output ports. The
metadata-profiler cut point stops before that function, so this document does
not claim a measurement for the cache.

The retained `proto/connection_node_identity` microbenchmark and the removed
source-copy bisection use different drivers and/or compiler settings. They are
exploratory records, not evidence for a main-branch representation change.
