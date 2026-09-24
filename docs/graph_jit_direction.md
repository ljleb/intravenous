# Graph JIT Direction

_Status: current design direction for synchronous whole-project graph compilation._

Related documents:

- [DSP Execution And Storage Glossary](./dsp_execution_storage_glossary.md)
- [project_graph_application_architecture.md](./project_graph_application_architecture.md)
- [sequential_port_storage_planning.md](./sequential_port_storage_planning.md)
- [coverage_and_background_evaluation.md](./coverage_and_background_evaluation.md)
- [builder_lowering_pipeline_design.md](./builder_lowering_pipeline_design.md)
- [intravenous-llvm-hot-reload-and-whole-graph-design.md](./intravenous-llvm-hot-reload-and-whole-graph-design.md)
- [event_flows/README.md](./event_flows/README.md)

## Responsibility

`GraphJit` is an application module whose single responsibility is to
synchronously transform one complete configured project graph into one optimized
native executable graph generation.

It owns the whole-project LLVM/ORC compilation domain. It does **not** own:

- canonical desired project state;
- node-definition or node-instance caches;
- project connection declarations;
- live `NodeStorage`;
- active audio-pass state;
- safe-point activation/state migration policy.

Those responsibilities remain with `ProjectGraph`, `NodeDefinitions`,
`NodeInstances`, `GraphConnections`, and `GraphExecutor` respectively.

The application boundary should remain LLVM-free. Runtime request/result/event
types and `CompiledGraph` metadata belong in runtime-facing headers; the concrete
`GraphJit` application-module implementation may include LLVM/ORC privately in
its own target/header. A PImpl is not required merely to keep LLVM out of
`ProjectGraph` or event users when the app-module boundary already provides that
isolation.

### Current implementation checkpoint

The application/compiler shell around whole-graph lowering has landed. `GraphJit`
now captures one exact `ConfiguredGraph`/`NodeDefinitionsSnapshot` generation,
resolves registered concrete nodes to the accepted package revisions that created
them, parses those revisions' finalized O0 bitcode, indexes compiler callbacks by
`NodeCodeKey`, resolves symbolic configuration-pointer relocations to retained LLVM
globals, and owns a persistent project `LLJIT` with independently releasable
per-generation resources. Generated project LLVM is verified, optimized at O3,
and materialized synchronously before an immutable `CompiledGraph` is returned.

The lowering boundary is executable for the semantic identity case: an empty
project lowers to an empty canonical `NodeLayout` plus a materialized no-op root
`tick_block` operation and returns a real `CompiledGraph`. This
proves the complete lowering -> verification -> O3 -> ORC -> native-operation
path without introducing special runtime storage or lifecycle machinery.

The first deliberately narrow non-empty slice has also landed. A flat project
may contain several registered zero-port primitives, with no connections,
nested declarations, or auxiliary declaration-owned regions. Configured
`virtual_nodes` records are treated as source/introspection metadata over the
already-lowered concrete bundles and endpoints; GraphJit does not instantiate
them as executable runtime nodes.
Configuration pointer fields are reconstructed from symbolic retained-global
relocations: native pointer bytes are discarded during host planning, selected
immutable retained globals are deduplicated as package import roots, and final
node configuration globals contain LLVM-relocatable pointers plus byte addends
(or explicit null pointers). Each primitive invokes its exact accepted
native `declare_node` callback into the one canonical `NodeLayoutBuilder`.
`State` and `IndexedState` are ordinary canonical `NodeStorage` regions:
generated root operations materialize each reflected callback context from final
layout offsets and dispatch the selected package LLVM against those live bytes.
Root execution now uses the connection-aware deterministic SCC/region schedule;
acyclic regions execute in dependency order and cyclic regions use slice-major
execution bounded by their derived feedback quantum. Callback imports are grouped
per package before a compile-local package module is consumed once, and repeated uses of one package
callback share one imported root while still receiving distinct node
configuration/storage contexts. The reflected compiler callback ABI uses
explicit pointer/count span records rather than assuming an
implementation-specific `std::span` object representation. The generated
Tick root exports only `tick_block`: it is the scheduler and may use primitive
`skip_block` callbacks internally when activity/skip semantics make that legal. A
separate Tick-root `skip_block` ABI would invert that ownership and is
intentionally absent. Graphs with background-evaluation work additionally export off-thread
forward, reverse, and evaluation batch roots. Those roots use the retained
`IndexedPlan` orders, imported reflected Tock callbacks, and the existing imported
`tick_block()` wrapper for replay; executor-owned batch frames still supply
coverage accumulators and page bindings.
Unsupported shapes still fail explicitly at the lowering boundary; they are
never compiled as no-ops.

General graph lowering remains the deliberately isolated compiler work. The
current lowerer is intentionally split at a stable internal phase boundary:
`lowering_plan.cpp` performs host-side graph inventory/validation, canonical
declaration/layout planning, package callback-import planning, immutable node
configuration planning, and root execution planning; `lowering.cpp` only
realizes that completed plan into LLVM and consumes the selected compile-local
package modules. Planning must succeed before the output module is mutated or a
package module is consumed. This is the first anti-monolith landing site for the
remaining compiler work.

Primitive maximum-block splitting has now landed on the existing execution
plan. Each primitive execution step carries its accepted maximum block size, and
LLVM realization owns the single slicing loop used by both tick and skip: a root
block is partitioned into consecutive primitive-sized slices with the sample
index advanced by each slice. This keeps subdivision out of callback/configuration
planning and gives port-context materialization one canonical per-slice invocation
boundary.

The second major landing-site refactor has now landed. Pure host-side connection
analysis lives in `graph_jit/connection_plan.{h,cpp}` and is intentionally usable
before LLVM/package realization. It inventories concrete nodes, classifies
connection access direction, derives Tick-to-Sequential scheduling dependencies, computes
deterministic Tick SCC/region ordering plus whole-project semantic cycle
reachability, records per-edge history/latency/conversion/boundary/feedback facts,
groups fanout by producer, derives the requirement
records consumed by the existing sample/event physical-storage choosers, and
emits semantic transient/persistent/external storage and liveness requests.
A tock-produced output never becomes a same-slice sequential dependency merely
because a sequential consumer reads its prepared pages. The source now uses
`SequentialInputConfig`/`RandomAccessInputConfig` for consumer access,
`TickOutputConfig`/`TockOutputConfig` for production, and separate
`OutputRetention::{ephemeral,persisted}`. Connection analysis classifies those
axes independently per sample source channel and event source/target pair. It
retains Tick -> Sequential dependencies for Tick scheduling, records Tock ->
Sequential preparation and random-access materialization separately, treats
persisted Tick outputs as stored boundaries, and proves contextual replay for
eligible Tick/ephemeral paths by traversing sequential dependencies. The retained
`IndexedPlan` keeps semantic SCCs separate from the background evaluation DAG and
records authored Tock execution versus synthesized replay. An unreproducible
Tick/ephemeral source feeding random-access demand requires an authored recorder.
The background executor itself has not landed at this checkpoint: Tock evaluation,
replay evaluation, page publication, missing-page neutral playback, and recording
consumption remain subsequent execution work.
Block-slice mismatches conservatively require materialization
until a later scheduler proves a shared subdivision. At the point this refactor
landed, the lowering capability gate still rejected ports/connections; the sample-
edge slice below was the first consumer of this analysis.

The sample-edge realization uses an important whole-project-JIT-specific ABI:
canonical `NodeStorage` contains only compiler-selected cross-call sample state,
never `SharedPortData`, `InputPort`, or `OutputPort` objects. GraphJit creates
per-node sample binding records containing capacities/layout facts and concrete
per-channel pointer/stride slices resolved by the generated root. Contiguous
representations currently populate those slices from one physical base, but the
ABI does not require channels of one logical port to share a representation. The
wrapper reconstructs
short-lived node-API `InputPort`/`OutputPort` values for that primitive invocation,
anchored to the absolute sample index. Those
facades have no cross-call identity; after whole-project inlining/O3 they are
expected to scalarize into address/index arithmetic. The direct and transient
materialization choices therefore allocate only bounded sample backing. There is
no audio-thread heap allocation, lazy initialization, placement construction,
persistent façade cursor, or `SharedPortData` tax.

The `choose_sample_connection_storage_plan()` and
`choose_event_connection_storage_plan()` functions are the physical-storage
policy boundary; GraphJit derives their requirement inputs and realizes their
returned choices rather than creating a competing policy layer. The old `Graph`
implementation is reference material only and must not constrain this runtime
representation. In particular, legacy fanout/cursor/storage objects should not be
carried forward merely to keep the old executor compiling.

The sample realization now has its own stable physical-plan layer in
`graph_jit/sample_physical_plan.{h,cpp}`. Primitive bindings refer to immutable
**sample representation handles**, not raw buffer identities or producer-group
storage objects. Every Tick producer group owns a canonical representation;
each realized
Tick-to-Sequential connection resolves to a representation handle. Identity Tick-to-Sequential
fanout branches resolve to that canonical representation. Point 8 adds explicit
derived transient representations for whole-port channel/layout conversions;
identical converted fanout branches share one derived representation and one
post-producer materialization operation without changing the primitive ABI.
Direct and transient-materialization representations are assigned exact byte ranges
inside one compile-time transient arena from their inclusive schedule live
intervals. The offline allocator tracks only currently-live ranges and places each
new representation in the lowest aligned free gap, so dead ranges can be split,
combined, and partially reused rather than leaving a historical whole-slot size
reserved. Equal-start allocations are considered size/alignment-first to reduce
fragmentation. Overlapping lifetimes never alias. The arena high-water mark and
all representation offsets are finalized before LLVM emission. The generated
root allocates that fixed arena in its stack frame and resolves each primitive
binding to a concrete representation pointer; audio-thread execution contains no
allocator bookkeeping or storage-class branch. The current packer is a
deterministic lowest-gap heuristic rather than a globally optimal interval-packing
solver; correctness and sub-range reuse are contractual, while globally minimal
arena size remains an optimization opportunity if measurements justify it.
Only compact carry and explicitly selected full persistent buffers belong in
`NodeStorage`. Event stack buffers are packed separately. Their intervals are
derived from the callbacks and merge/materialization/feedback operations that
actually access them rather than from the complete producer-group interval, so
producer-local and converted buffers can reuse stack bytes as soon as their last
reader has run. Producer overflow counters remain independent persistent
telemetry.

The physical planner consumes the storage decision already made by
`choose_sample_connection_storage_plan()`; it does not choose policy again.
Only Tick-to-Sequential branches receive sequential-storage representation handles
here; random-access and background-prepared branches remain unresolved for the later
background evaluation component executor. Whole-
port conversion is channel-granular: layout-only conversion and mono-to-stereo
duplication bind existing producer channels directly, while arithmetic conversion
materializes only its computed result channels. Semantic channel projection and
permutation preserve each source channel's physical producer identity and
independent read latency while complete target contributions normalize into
canonical target order. Feed-forward conversion kernels read resolved semantic
channels directly, including channels backed by different producer
representations, rather than gathering a contiguous conversion input. GraphJIT
uses `ChannelConversionRegistry` to validate supported semantic conversions but
does not invoke its contiguous-block conversion callback for these operations.
Detached composition writes into a persistent feedback timeline, including
conversion, permutation,
unequal-latency alignment, and migration. This does not reintroduce connection
helper nodes. External boundaries remain capability-gated rather than being
approximated with transient storage.

### Tick/Sequential port realization rules

The remaining port work should preserve these invariants:

- **Storage contains data, not API facades.** Invocation-local sample/event
  payloads use the fixed generated-root stack frame. History/latency carry, full
  persistent buffers, and genuinely cross-call implementation state use
  `NodeStorage`. `InputPort`/`OutputPort` are invocation-local authored-node API
  adapters over the selected concrete pointer.
- **Bindings are immutable compiler facts.** Port implementation kind, offsets,
  capacities, layouts, history/latency parameters, and branch relationships should
  be emitted as immutable LLVM-visible records whenever possible. The generated
  root resolves each binding to its already-selected stack or `NodeStorage`
  pointer before invoking the node; fetching a port must not branch between
  storage classes. Only concrete base pointers and the current sample index/block
  size are dynamic values.
- **No audio-thread setup.** Root execution performs no heap allocation, lazy
  initialization, ownership changes, or first-call construction. Compiler-owned
  persistent sample/event state declares `NodeLayout` raw-region initializers and
  is initialized (or exact-shape migrated) by `NodeStorage` before activation.
  Per-root sequence resets and SCC cursors are execution semantics, not deferred
  graph construction. Any bounded invocation-
  local facade values are ordinary inline/stack/SSA values generated by the
  imported wrapper and are not lifecycle-managed runtime objects.
- **Producer groups own physical representations.** Fanout consumers reference one
  producer-group representation or explicit derived branches; there is no default
  one-buffer/one-object-per-edge model.
- **Conversions and fanout materialization are explicit physical choices.** A
  producer writes its canonical source-layout representation once. Identity and
  aliasable converted/remapped branches bind its channels directly; arithmetic
  conversions use planned derived-result operations. Conversion is not hidden as
  mutable state inside `OutputPort`.
- **Transient and persistent state stay distinct physically and semantically.**
  Current-block scratch is stack-frame storage and never migration state.
  `NodeStorage` holds either the exact carry crossing calls or an explicitly
  selected full persistent port buffer.
- **Direct is allowed to have backing.** With the authored node API, producer output
  still needs an addressable current-block representation. `direct` means no extra
  connection copy/materialization between producer and consumer. A later fusion/SSA
  optimization may eliminate even that backing where profitable.

The shell continues to use the generated-root and canonical
`NodeLayout`/`NodeStorage` contract specified in this document: `CompiledGraph`
carries the finalized `NodeLayout` plus the generated root `tick_block`, while
lifecycle remains entirely in ordinary `NodeStorage`. Primitive `skip_block`
callbacks are internal scheduling tools, not root operations. Whole-project
lowering must not reintroduce a second node-storage
layout, a second lifecycle system, or a synthetic project-wide coverage/Tock ABI merely to expose internal Tock outputs.

### Current connection capability audit

The current internal Tick/Sequential connection surface is intentionally asymmetric:

- **Samples:** ordinary internal Tick-to-Sequential transport is complete across direct and
  transient storage, fanout/deduplicated conversion, channel composition and
  projection/permutation, declared history/output latency, whole-graph latency
  compensation, compact carry/persistent rings, and `detach()` SCC feedback. A
  sample producer inside an SCC may also fan out to downstream acyclic identity or
  converted/history-bearing consumers. What remains is not another ordinary sample
  transport mode: mixed Tick/background and background-only delivery are planned
  by the background-evaluation topology and still require the executor work in
  coverage/background points 14-19 below. Root I/O is not a boundary-connection
  mode; it is expressed by concrete system/communication node types.
- **Events, feed-forward:** internal Tick-event direct/transient sequences, block
  adaptation, non-expanding conversion, fanout, stable multi-producer fan-in,
  compact retained carry, persistent rings, and retained converted fanout are
  implemented. Independent producers write bounded local sequences which are
  stable-merged in semantic source order after the final producer, so equal-time
  event ordering is deterministic before conversion/retention/fanout.
- **Events, cyclic:** event operations now carry their execution scope in the
  physical plan. Primitive-scoped operations run for every SCC slice;
  region-scoped operations run once at region entry or exit. This supports
  cyclic source history/latency, ingress and inter-region streams, split fanout
  scopes, same-SCC fan-in, converted/history-bearing feedback, and disconnected
  ports without execution planning inferring a window from buffer consumers.
- **Both kinds:** the root graph is required to have zero public/boundary ports.
  Device I/O and communication with other application modules enter through
  concrete node types, so there is no future root-boundary transport ABI to add.
  Random-access and background-prepared directions remain a separate lowering capability.

#### Tick/Sequential sample capability matrix

| Connection shape or feature | Current state | Physical behavior or remaining requirement |
| --- | --- | --- |
| One-source, exact-layout, zero-retention feed-forward | Implemented | Compatible consumers alias the producer representation directly. The producer still has addressable current-block backing, but the connection adds no copy. |
| Producer/consumer block-size mismatch | Implemented | The physical plan places the required block materialization before or after the relevant primitive while preserving absolute sample indices. A sliced producer can accumulate a root-call representation for unsliced or differently sliced consumers. |
| One producer with multiple consumers | Implemented | Identity fanout aliases one canonical representation. Equivalent converted branches share derived result channels and materialization work. |
| Channel projection, permutation, or duplication | Implemented | Each target channel binds directly to its resolved source channel and frame delay. Layout-only conversion does not gather or copy a synthetic contiguous input buffer. |
| Arithmetic channel/layout conversion | Implemented | Conversion reads the resolved semantic source channels and materializes only result channels that cannot be expressed as aliases. |
| Multi-source channel composition/fan-in | Implemented | The target layout is resolved channel by channel. Aliasable channels remain direct; arithmetic mixing/conversion materializes only the affected result channels, with latency alignment applied before composition. |
| Source history, output latency, and target read history/latency | Implemented | Timing analysis derives the exact retained horizon. Small horizons use stack working storage plus persistent carry; larger horizons use a full persistent timeline. Consumers retain one unconditional resolved-channel lookup path. |
| Unequal feed-forward path latency | Implemented | Whole-graph cumulative latency analysis assigns compiler-owned read compensation to faster branches before conversion, composition, projection, or fanout. |
| Detached feedback within one SCC | Implemented for internal Tick-to-Sequential samples | A fixed-capacity delayed timeline uses the same transient/carry/full-storage alternatives. Producer-home and branch-local writers support history, latency, conversion, permutation, composition, and nonzero `loop_extra_latency`. |
| Cyclic producer with ordinary downstream fanout | Implemented | The SCC timeline is updated slice by slice; downstream identity or converted/history-bearing branches are realized at the scope where the completed SCC result becomes available. |
| Acyclic ingress or an edge between execution regions | Implemented | Explicit before/after materialization placement carries the resolved channel representation across the schedule; persistent storage is used only when the semantic history/latency lifetime crosses root calls. |
| Mixed Tick/background or background-only sample delivery | Planned; Tick lowering capability-gated | The connection/background plan now preserves each contribution independently. Playback/materialization belongs to background execution rather than another sequential sample-buffer representation. |
| Unconnected sample port in Tick lowering | Implemented | An unconnected input binds to compiler-emitted constant sample data filled with its declared `default_value`. An unconnected output receives an ordinary writable buffer sized from its declared history/latency; retained samples use the same stack-plus-`NodeStorage` or full-`NodeStorage` choice as connected outputs. |

#### Tick-event SCC capability matrix

| Connection shape or feature | Current state | Physical behavior or remaining requirement |
| --- | --- | --- |
| Same-SCC, one-source, zero-retention exact-type feed-forward | Implemented | The cyclic producer appends into one aggregate sequence across all root-call slices. Same-region consumers select their current absolute-time slice directly. |
| Same-SCC non-expanding conversion | Implemented | A derived sequence is materialized after each producer slice, before its in-region consumer. |
| Same-SCC target history | Implemented | Canonical retained storage keeps restored root history and earlier-slice events visible. Exact-type consumers read that storage directly; a converted derived branch materializes `[slice-history, slice-end)` after each producer slice. |
| Same-SCC source history | Implemented | A history-bearing cyclic output writes one bounded invocation-local sequence. A slice-scoped stable merge inserts it into the retained canonical aggregate, so an event authored behind the previous slice tail does not violate append ordering. |
| Same-SCC source latency consumed by a non-detached target | Implemented | The same invocation-local merge keeps future-authored events sorted in the canonical retained timeline; exact consumers filter it directly and converted consumers receive a slice-scoped view. |
| Detached feedback within one SCC | Implemented for Tick sources | The delayed live span includes source/target history, authored latency, and `loop_extra_latency`, then selects the shared transient/carry/full storage planner. Exact consumers alias the delayed stream; non-expanding conversions materialize before the consumer slice. Temporal sources feed feedback from an invocation-local stream, so only newly authored events are delayed, and bounded insertion preserves time order across overlapping history. |
| Cyclic producer to acyclic consumer | Implemented | Materialize once at SCC exit from the complete root-call aggregate. Exact type, non-expanding conversion, outbound target history, and authored source latency compose with compact carry or a canonical persistent ring. |
| Acyclic producer entering a cyclic region | Implemented | Exact-type consumers read the completed root-call aggregate directly. Converted ingress is materialized once at target-region entry. |
| Edge spanning distinct cyclic regions | Implemented | The source aggregate remains live across regions. Conversion runs at source-region exit and the downstream region reads its absolute-time slices. |
| Multi-producer fan-in touching a cyclic region | Implemented | Acyclic producers merge once after their completed invocation; each cyclic-region stage merges bounded producer-local streams after that region's final producer on every slice. A compiler-private source-ordinal sidecar on the canonical aggregate preserves semantic equal-time ordering even when execution-region order differs from source order. Compact carry and persistent rings retain the ordinals with their events. |
| One derived materialization consumed both inside the source SCC and downstream | Implemented | Representation sharing is keyed by conversion and execution scope. The in-SCC and SCC-exit branches receive distinct scope-correct derived representations. |
| Mixed Tick/background or background-only event delivery | Planned; Tick lowering capability-gated | Per-source/per-target delivery is retained in the background plan. Playback/materialization belongs to background execution rather than another sequential-storage kind. |
| Unconnected primitive event port | Implemented in Tick lowering | Inputs receive a reset zero-capacity sequence. Outputs receive a bounded sink sized from `max_events_per_index`, history, latency, and root block size, with normal overflow telemetry. |

#### Remaining event-connection work

The remaining work should be treated as compatibility between semantic windows,
execution regions, and the existing physical representations—not as a request
for one universal event buffer.

Remaining semantic capability work:

1. Lower the already-planned mixed Tick/background and background-only event
   deliveries through the background executor and prepared-playback path.

Efficiency and observability work that does not change event semantics:

- **Landed:** replace the sample/event implementation enums with the three
  storage plans—transient stack, transient stack plus persistent carry, and full
  persistent `NodeStorage`—while keeping aggregation/conversion/feedback as
  separate operation facts;
- make every event capacity a required compile-time result of
  `max_events_per_index` and the exact simultaneously-live temporal span; an
  invalid/unrepresentable result must fail compilation rather than select a
  fallback;
- **Landed:** move invocation-local sample and event backing out of
  `NodeStorage` and into live-range-packed generated-root stack arenas, with
  direct resolved-pointer bindings and no runtime storage-kind branch;
- **Landed:** plan feedback as an ordinary delayed derived stream using the same
  transient/carry/full alternatives, with event feedback sized from
  rate-times-live-span rather than `source_capacity * (latency + 1)`;
- **Landed for acyclic exact-type fan-in:** retained fan-in now compares the
  separate canonical aggregate with producer-home using whole-group copy and
  stack costs. When authored timing preserves append order and producer-home is
  cheaper, semantic source 0 writes directly into the restored/pruned canonical
  sequence and only the remaining producer-local streams are merged;
- surface the existing per-logical-output saturating overflow counters; and
- **Landed:** the shared chooser now enumerates and scores all three
  candidates from copied bytes, addressed bytes, stack footprint, persistent
  footprint, and a configured stack bound while preserving the old crossover
  under default weights. Acyclic event fan-in supplies topology-local sequence
  footprints and stable-merge copy counts, including producer-home alternatives.
  `GraphJitConfig` now supplies one cost model to sample, event, and feedback
  planning, and lowering enforces its stack limit against the final packed
  sample+event stack allocation, re-planning eligible buffers into `NodeStorage`
  when necessary. Event lowering then costs the concrete shared conversion and
  feedback operations before final residence realization: conversion output
  writes are counted once, full-persistent source reads and delayed-stream ring
  writes are explicit, compact feedback includes restore/commit copies, and
  identity fanout adds no copy. The remaining work is alias-versus-materialize
  comparison, weight calibration, and refining which eligible buffer is moved
  when several alternatives can satisfy the same limit. The fixed 64-event and
  16-KiB thresholds have been removed.

#### Event-connection implementation map

Use these files as the phase boundaries when extending the matrix:

- [`graph/realtime_port_planning.h`](../src/intravenous/graph/realtime_port_planning.h)
  contains the shared storage-kind vocabulary, payload-specific requirement
  records, and pure policy choosers. It must not acquire topology-specific
  lowering logic.
- [`graph_jit/connection_plan.h`](../src/intravenous/graph_jit/connection_plan.h)
  and [`connection_plan.cpp`](../src/intravenous/graph_jit/connection_plan.cpp)
  derive logical event connections, producer groups, SCC schedule facts,
  retention requirements, and implementation choices before physical lowering.
- [`graph_jit/lowering_plan.h`](../src/intravenous/graph_jit/lowering_plan.h)
  and [`lowering_plan.cpp`](../src/intravenous/graph_jit/lowering_plan.cpp)
  realize event representations and operations, apply explicit capability
  gates, and place work at primitive or region entry/exit scope.
- [`graph_jit/event_conversion_runtime.h`](../src/intravenous/graph_jit/event_conversion_runtime.h)
  and [`event_conversion_runtime.cpp`](../src/intravenous/graph_jit/event_conversion_runtime.cpp)
  own bounded conversion, visible-window materialization, and stable merge
  leaves used by generated code.
- [`graph_jit/event_retention_runtime.h`](../src/intravenous/graph_jit/event_retention_runtime.h)
  and [`event_retention_runtime.cpp`](../src/intravenous/graph_jit/event_retention_runtime.cpp)
  own compact-carry restore/commit, persistent-ring pruning, and feedback suffix
  append behavior.
- [`node/build_request.h`](../src/intravenous/node/build_request.h) and
  [`ports.h`](../src/intravenous/ports.h) reconstruct invocation-local event
  facades over the compiler-selected raw representation. They are the authored
  node API contract, not the physical-policy layer.

For a new combination, first extend semantic facts and scheduling legality, then
choose one of the three storage plans for each canonical or derived
representation. Identity aliasing, merge, conversion, block/SCC adaptation, and
feedback delay are explicit operations over those representations rather than
additional storage kinds.

Sample output authored latency is also a revision horizon. `OutputPort::update()`
may rewrite any of the preceding authored-latency frames, so canonical sample
storage retains that horizon across root calls. Detached copy/composition writers
unconditionally refresh the available authored-latency prefix together with the
current block; producer-home feedback needs no copy because revisions already hit
the persistent canonical ring directly.

The sequential callback contract is part of lowering correctness, not merely a
node-helper detail. A reflected sample facade is reconstructed at the primitive
invocation's absolute `sample_index`. For a native `tick_block()` callback, input
cursors remain anchored there for the duration of the callback and block accessors
address later frames explicitly. Sequential `OutputPort::push*()` calls advance
the output's authored cursor, while static/direct block writes are committed once
when the callback returns. A node that implements only `tick()` is executed by
`do_tick_block()` as one one-sample context per frame, advancing input/output
cursors after every call. Primitive maximum-block slicing reconstructs the same
facades at each slice index, so authored-latency revision must remain valid across
both slice and root-call boundaries. Well-formed Tick nodes publish exactly
one sample frame per output per `tick()`, or `block_size` frames per output per
`tick_block()`; release execution does not maintain a redundant production-count
check.

`skip_block()` uses the same block anchoring. A custom skip callback owns its own
output semantics; when one is absent the generic helper synthesizes silence for
every channel of every sample output and advances inputs by the skipped block.
GraphJit's generated root does not yet schedule primitive skips, so this remains a
generic callback contract until activity/TTL lowering lands.

Fresh compiler-owned persistent connection state is lifecycle-owned. Sample
feedback/carry/alignment raw regions and persistent event representations install
`NodeLayout` raw initializers; exact-shape persistent regions skip initialization
when migration restores their bytes. The generated Tick root contains no
first-call initialization guard. Transient event sequences and feedback cursors
are fixed stack/SSA state for one root call and reset there. Unequal-latency
sample-feedback alignment uses the initialized alignment-ring samples directly as
branch prehistory. As real source frames arrive they overwrite those slots
naturally, so no validity counter, warmup branch, or post-activation
initialization state is required.

### Ordered implementation sequence

The implementation order is intentional. Each structural refactor should land
before the feature family that depends on it, so whole-graph lowering does not
accumulate one-off paths that must be disentangled later.
This is a hint, not a hard constraint. Use your own good judgement if ever in doubt.

1. **Stable node-lowering phases.** **Landed.** Separate host-side inventory/analysis,
   declaration/layout planning, package callback-import planning, configuration
   planning, execution planning, and LLVM realization without widening the
   accepted graph shapes.
2. **Multiple zero-port primitives.** **Landed.** Add deterministic execution order,
   multiple canonical declarations/configurations, callbacks from several
   packages, and several selected callbacks from one package. Preserve per-primitive
   `skip_block` legality as scheduler metadata; the root itself exports only
   `tick_block`.
3. **Configuration relocations.** **Landed.** Reconstruct configured pointer fields
   from already-resolved retained-global relocation records. Retained globals are
   imported as deduplicated package roots, native pointer bytes are zeroed during
   planning, and immutable node configuration LLVM contains symbolic pointers,
   byte addends, and explicit null slots rather than native process addresses.
4. **Primitive maximum-block splitting.** **Landed.** Primitive execution steps
   carry their accepted maximum block size, and one LLVM-emission path slices
   both tick and skip invocations while advancing sample indices correctly.
5. **Stable connection-analysis plans.** **Landed.** Pure host-side planning now
   records node/dependency topology, deterministic SCC/region scheduling,
   producer-group/connection temporal facts, sample/event chooser requirements,
   and semantic liveness/storage requests before any package LLVM is consumed.
   Sequential-storage policy selection remains in
   `choose_*_connection_implementation()`; random-access and mixed-delivery
   directions are classified separately. Configured
   virtual-node records are metadata only: execution planning follows concrete
   bundles and configured connections directly and never lowers legacy/internal
   virtual/runtime helper nodes.
6. **Simple feed-forward sample connections.** **Landed.** Internal whole-port
   Tick-to-Sequential sample edges realize `direct` and `transient_materialization` with
   bounded sample backing only. Reflected binding records hold concrete resolved
   per-channel pointer/stride slices; imported primitive wrappers reconstruct
   invocation-local `InputPort`/`OutputPort` facades from those pointers and the
   absolute sample index.
   No sample facade, cursor object, `SharedPortData`, or raw-region initializer is
   stored in `NodeStorage`.
7. **Stable sample-representation realization.** **Landed.** The point-6
   one-buffer-per-producer realization has been replaced by a producer-group
   physical plan with immutable representation handles, canonical producer
   representations, per-connection representation resolution, explicit transient
   lifetime semantics, and deterministic aligned byte-range packing in one
   transient arena. Dead ranges are reusable at sub-range granularity, including
   partial holes left by differently-sized representations. Primitive bindings no
   longer encode raw buffer identity. No retained
   representation is faked with transient storage; persistent/feedback/external
   kinds remain capability-gated for their dedicated later steps.
8. **Sample fanout, layout conversion, and channel composition.** **Landed for
   feed-forward Tick-to-Sequential branches.** One canonical producer-layout representation
   is written once; identity, layout-only, and mono-to-stereo consumers bind its
   channels directly. Arithmetic converted fanout still uses a deduplicated derived
   result representation. Pure semantic channel projection/permutation and
   aliasable conversion bind target channels directly to resolved producer channel
   slices, preserving each producer's independent capacity and read latency without
   gathering a synthetic target-layout representation. Mixed compositions are
   channel-selective: aliasable channels remain direct, while arithmetic conversion
   reads distinct resolved source channels and materializes only its result channels.
   Materialization is generated
   whole-project LLVM using absolute-position-addressed physical storage and contains no
   runtime converter object, heap allocation, or `OutputPort` conversion state.
9. **Sample history and latency.** **Landed for declared sample history/latency in Tick execution.**
   `compact_persistent_carry` uses one transient absolute-position-addressed working ring plus
   exactly the retained tail in persistent raw `NodeStorage`; the tail is restored
   before its producer and committed after producer-side materializations. Larger
   retention uses `persistent_ring`, binding primitives directly to one power-of-two
   persistent ring. Immutable input bindings carry authored history/read latency;
   aliasable converted branches read retained producer history directly, while
   arithmetic derived results materialize the historical window they actually need.
   Both modes use absolute sample-index addressing. Persistent compiler-owned raw regions
   carry stable migration identities and exact-shape `NodeStorage` migration copies
   their bytes across generations; transient arenas never migrate. This point covers
   declared output latency and input/output history. Feed-forward whole-graph
   path-latency equalization is also landed: cumulative node/internal/output latency
   propagates through the schedule, faster branches receive compiler-owned read
   compensation, and that timing survives conversion, fanout, channel composition,
   history, and target-channel projection/permutation. Feedback/SCC latency is
   realized by point 12 below.
10. **Event-port realization refactor and simple event flow.** **Landed for direct, transient block adaptation, conversion, feed-forward fanout, and multi-producer fan-in.**
    Events use compiler-planned bindings over bounded representations; imported
    primitive wrappers reconstruct invocation-local event facades rather than
    persisting `EventSharedPortData`/port objects. Transient representations are
    lifetime-packed into a fixed generated-root stack arena, while bindings hold
    already-resolved pointers to stack or persistent storage. Exact-type, zero-retention,
    unsliced Tick producer groups realize direct bounded sequences. Sliced
    producers use a bounded invocation aggregate. Exact-type consumers alias it
    and select their absolute-time window without a copy; conversion creates an
    explicitly scoped derived sequence.
    Event conversion plans are preserved by semantic analysis and realized as
    explicit transient sequence operations; identical converted fanout branches
    share one derived representation/materialization. Tick event outputs are
    contractually emitted in nondecreasing absolute sample-index order within
    each callback invocation. Transient
    multi-producer event inputs place semantic source 0 directly in the canonical
    aggregate allocation, keep the remaining producers in bounded local
    sequences, and perform one stable backwards k-way merge after the last
    producer completes. Retained acyclic fan-in may instead select semantic
    source 0 as the canonical producer-home when whole-group costing and authored
    timing make that realization legal. Retained aggregate storage, conversion,
    and fanout all operate downstream of the merge. Fan-in spanning several
    execution regions instead stages producer-local streams into one canonical
    aggregate as each region makes them available. Its compiler-private
    source-ordinal sidecar lets later stages insert equal-time events at their
    semantic source position while authored ports continue to see an ordinary
    contiguous `TimedEvent` sequence.
    Implicit conversions are intentionally non-expanding: one source event may
    produce zero or one target event, never synthesize additional events.
11. **Event retention.** **Compact carry and persistent-ring identity retention landed.**
    Small retained windows use a transient working sequence plus a migration-identified
    persistent raw carry. The carry is restored before the producer, producer slices
    append into the working sequence, and commit filters exactly the next root
    invocation's `[end-history, end+latency)` window while preserving absolute event
    timestamps. Larger retained windows bind producer and consumers directly to one
    migration-identified persistent power-of-two ring. The ring stores monotonic
    read/write indices and expires only events older than the current retained-history
    boundary before producer execution, so no retained tail is copied at root-call
    boundaries. Event outputs declare a finite nonnegative `max_events_per_index`
    static sizing rate in `EventOutputProperties`; GraphJIT combines that rate with
    each representation's temporal span to derive static capacities and uses the
    retained representation capacity as input to physical-plan comparison. The
    declared maximum bounds total events in the represented window without
    constraining their timestamp distribution. Exceeding it has
    implementation-defined behavior and must never grow storage or allocate on
    the audio thread. The current implementation owns one saturating overflow
    counter per logical output and drops an event when its bounded producer
    representation is full; that response is not part of the port contract.
    Retained canonical representations may now feed transient consumer branches:
    compact-carry working sequences and persistent rings materialize the current root
    window plus each branch's declared input history, then apply the existing
    non-expanding conversion plan. Identical retained converted fanout branches share
    one transient representation. Derived capacity remains source-capacity-sized
    because the declared maximum does not require events to be distributed
    uniformly across timestamps. Event feedback storage lands in point 12; telemetry
    surfacing remains; transient event backing and direct-pointer binding are landed.
12. **SCC/feedback execution.** **Sample and Tick-event feedback landed.**
    Sample `detach()` now executes through feedback-aware SCC
    scheduling with nonzero reflected `scc_feedback_latency`, producer-home or
    branch-local retained timelines selected through the shared storage planner,
    source latency/history, channel conversion,
    projected/permuted composition, unequal-latency mixing alignment, exact-shape
    generation migration, and ordinary identity/converted/history fanout from an
    SCC producer into downstream acyclic regions. For internal Tick-to-Sequential sample
    connections this closes the normal transport surface; the remaining sample
    connection gates are random-access/background delivery directions; root I/O is represented by
    ordinary concrete system/communication nodes rather than boundary ports.
    Event feedback accepts Tick-produced exact or non-expanding converted streams,
    including source/target history, source latency, and same-SCC fan-in. The
    implementation covers same-delay
    detached fanout, burst retention, changing root-call sizes, and generation
    migration. Compact-carry feedback cursors skip the restored historical prefix;
    persistent-ring cursors start at the prior monotonic write index. Both paths
    therefore copy only events authored during the current root call rather than
    re-enqueueing retained history or future events. A cyclic producer may also
    fan out to an acyclic consumer through one SCC-exit materialization, including
    non-expanding conversion and target history backed by compact carry or a
    canonical persistent producer ring. Same-SCC non-detached conversion and
    temporal consumption run after each producer slice. Ingress conversion runs
    at region entry, inter-region conversion at source-region exit, and mixed
    in-SCC/downstream fanout receives distinct scope-correct derived
    representations. The audit matrix above is authoritative for the remaining
    combinations.
13. **Root I/O node integration.** Keep the configured project root zero-input and
    zero-output. Device I/O and communication with other application modules are
    ordinary concrete node definitions that own the relevant external resource or
    application-module bridge; GraphJit must not add a root boundary-binding ABI.
14. **Landed: legacy generated-node graph executor and dynamic concrete-port
    schema fallbacks deleted.** `ModuleLoader` publishes `ConfiguredGraph` and
    derives source introspection directly from it. The `GraphLowerer`/
    `GraphCompiler`/`RuntimeGraphRoot` path, obsolete generated routing nodes,
    type-erased runtime facades, old root ABI and legacy callback-context spans are
    gone. Static constexpr concrete-node ports are enforced in both public
    `IV_NODE` and internal builder entry points; dynamic topology, instances and
    per-instance connection metadata remain supported.
15. **Landed: independent port contracts and generalized connection planning.**
    Input access, output production, and output retention remain distinct through
    the builder, serialization, compiler records, per-channel tiling and GraphJit
    planning. Connection compatibility is classified per source channel/event pair;
    only Tick -> Sequential contributes same-slice scheduling and sequential storage,
    while Tock preparation/materialization and persisted Tick boundaries are retained
    as background facts. Unreproducible Tick/ephemeral -> RandomAccess demand is the
    remaining connection-level rejection and requires explicit recording.
16. **Landed: replayability trait and contextual replay planning.** The trait opts in
    an existing `tick()`-only node with no `State`, random-access inputs, history or
    latency and a fixed-version pure/deterministic contract. GraphJit retains the
    generated `tick_block()` import, proves upstream availability through ordinary
    Sequential dependencies, stops at persisted boundaries, rejects replay cycles
    and records synthesized pointwise F/R plus background replay ordering. Contextual
    replayability remains a per-path compiler fact, not an output config field.
17. **Finish ordinary background evaluation before capture consumption.**
    The reusable reflected batch ABI, coverage/Tock callback imports, and generated
    F/R/evaluation programs have landed. Next implement `GraphExecutor`,
    transaction-local Tock materializations, persisted page completion, atomic publication, pinned
    snapshot reads and advance preparation for sequential playback. Tock-to-random-
    access edges require a transaction-local page-backed materialization when the
    Tock output is ephemeral;
    eligible pure-tick subgraphs can be fused. Playback reads stale pages as-is and
    substitutes an input's neutral value for missing pages without blocking.
18. **Enable explicit recording.** Define bridge capture metadata and authored
    recording policy, provision slab storage off the audio thread, then capture
    immediately at the producing block. Consume fixed capture prefixes through the
    **already implemented** F/R/evaluation transactions; publish once and reclaim
    committed capture blocks. Invalidation never evicts persisted covered data.
19. **Generation reconciliation.** Rebind compatible stable persisted stores across
    generations. Persisted generated/finalized data remains retained throughout its
    covered lifetime; coverage removal is the only semantic deletion condition.
    Superseded physical versions are reclaimed after their readers release them.
20. **Finish remaining authored-node semantics** (nested declarations, compiler-
    owned regions, activity/TTL, detach, events and skip scheduling).
21. **Optimize** with SIMD, loop fusion, materialization placement, storage liveness
    and bounded immutable-value specialization only after correctness is established.

The detailed, normative dependency order is
[coverage_and_background_evaluation.md §32](./coverage_and_background_evaluation.md#32-implementation-landing-order).
The two compiler JIT **stages** below must not be confused with the deprecated
legacy graph **executor**, which is to be removed completely.

## Two sequential JIT stages, two ownership domains

The application intentionally has two JIT stages with different purposes.

The existing package/configuration JIT is analogous to a compile-time
metaprogram JIT:

```text
package C++ / retained package LLVM
        |
        v
package/configuration JIT
        |
        | execute registered construction/configuration callbacks
        v
ConfiguredGraph structures and configured values
```

The project graph JIT consumes the resulting graph description and the retained
primitive implementation LLVM:

```text
ConfiguredGraph
+ exact provider/code provenance
        |
        v
whole-project lowering and explicit graph analyses
        |
        v
specialized root-node LLVM + background-evaluation component executors
        |
        v
GraphJit ORC
        |
        v
native CompiledGraph
```

The first JIT executes configuration code. The second JIT compiles the DSP
program described by that configuration.

The native DSP output of the first JIT is **not** the preferred code-generation
input to the second JIT. `GraphJit` imports the required callback closures from
the retained primitive node LLVM associated with the exact providers used by the
configured graph so whole-project inlining and optimization remain possible. The
accepted native node declaration/lifecycle callbacks remain useful for the canonical
`NodeLayout`/`NodeStorage` contract and must stay pinned by the exact accepted
package revisions.

The two JITs may share non-app-module LLVM utility code, target setup, optimizer
helpers, object-cache helpers, or memory-manager helpers. They should not share
application-module ownership merely because both use ORC.

## Project-JIT ownership

`PackageJit` owns the persistent `ModuleLoader` and therefore the shared
package/configuration ORC state. A shared package `LLJIT` survives individual
package revisions, while each accepted `PackageRevision` pins its
revision-specific package code/resources and finalized compiler artifact.

The whole-project graph JIT uses the analogous lifetime pattern in its own
domain:

```text
GraphJit
    |
    v
shared project LLJIT
    |
    +-- project generation N JITDylib/resources
    +-- project generation N+1 JITDylib/resources
    +-- project generation N+2 JITDylib/resources
```

A single `LLJIT` owned by `GraphJit` may live for the application lifetime. Each
compiled project generation has independently releasable ORC resources through a
generation-specific `JITDylib`/`ResourceTracker` lifetime object.

Old and new generations must be able to coexist while `GraphExecutor` finishes a
pass, prepares `NodeStorage` migration, or retains active/pending generations.
A `CompiledGraph` therefore pins its code/resource lifetime rather than exposing
naked function pointers whose lifetime is implicit.

## Synchronous compilation is the intended contract

Whole-project graph compilation is intentionally synchronous inside one
`ProjectGraph` rebuild transaction.

The target is for it to be fast enough that introducing an asynchronous compiler
pipeline would add more consistency/lifetime machinery than value. The compiler
should do graph-specific work before LLVM so the final LLVM module is already
close to the desired native program.

Conceptually:

```cpp
GraphJitCompileResult GraphJit::compile(GraphJitCompileRequest const& request);
```

and the caller immediately continues with the returned result in the same
propagation cause.

If whole-project compilation later becomes unexpectedly expensive, treat that
as a compiler-performance problem to measure first. Do not pre-commit the
architecture to asynchronous compilation merely to hide avoidable compiler
work.

Package discovery/build/reload may still be asynchronous because that is a
different source-build domain. The synchronous rule here applies specifically
to the final configured project graph -> native graph generation step.

## Exact-generation consistency

`GraphJit` must compile exactly the provider/code generations used to construct
the supplied `ConfiguredGraph`.

It must **not** ask `NodeDefinitions` for whatever definitions happen to be
current when compilation begins. Doing so would both create an unwanted
application-module dependency and make one project transaction vulnerable to a
mixed definition generation.

The compile input therefore carries the same immutable definition generation
used to construct the graph, and configured registered nodes carry exact
provider/code identity. Each accepted `PackageRevision` pins both its native
callbacks/lifetime and the finalized O0 bitcode used by the project JIT.

The invariant is:

> One root graph is configured, declared, and compiled against one coherent
> definition world.

## The compiled project masquerades as one root node

The optimized project graph should use the existing root-node execution model
instead of defining a parallel project-kernel object model.

The generated project root has no public inputs or outputs:

```text
project root
    inputs  = {}
    outputs = {}
```

This matches the existing `BlockNodeExecutor` root contract. The optimized root
keeps ordinary node semantics, but declaration is consumed as a compile-time
layout contract rather than materialized as a runtime JIT entrypoint. Runtime
behavior is therefore:

```text
compile time: declare/layout planning
runtime:      NodeStorage initialize()/move()/release()
runtime:      generated tick_block() (owns child tick/skip decisions)
```

The constituent nodes retain their own declaration and lifecycle semantics.
Declaration is performed during lowering, before final LLVM emission: the compiler
invokes each exact accepted native `declare_node` callback into one
`NodeLayoutBuilder`, adds compiler-owned raw regions, then finalizes the
`NodeLayout`. This preserves each node's state structures, lifecycle callbacks,
dependencies, and nested-state relationships while making every final storage
offset available as a constant to LLVM. `NodeStorage` remains responsible for
invoking initialization, move/migration, release, and destruction in the order
described by that layout. Do not replace that orchestration with monolithic
generated project initialize/move/release functions.

The root node has no project-visible background-evaluation output ports, therefore it has no
project-wide `tock_coverage()` operation. Internal requestable outputs remain
addressable through immutable `CompiledGraph` metadata described below; they are
not exposed by pretending that the zero-port project root has synthetic outputs.

## One canonical fixed-layout `NodeLayout` and `NodeStorage`

There is exactly one **fixed-layout** persistent storage model for an executable
graph generation: the existing `NodeLayout`/`NodeStorage` machinery. This does
not mean every request-sized or dynamically growing runtime object belongs in
`NodeStorage`.

`GraphJit` must not introduce `CompiledGraphNodeStorageLayout`,
`GraphKernelStorage`, or another parallel fixed state arena. Lowering populates
one `NodeLayoutBuilder` and finalizes it before emitting final storage accesses
into LLVM. The completed `NodeLayout` becomes part of `CompiledGraph`, and
`GraphExecutor` creates and owns the corresponding `NodeStorage`.

The canonical fixed layout may include `IndexedState` as well as
normal `State`, but their semantics differ. `IndexedState` is Tock-side acceleration state. `State` participates in sequential
node behavior. `IndexedState` is optional non-semantic acceleration storage exposed
only to `tock_coverage()`; tick and propagation callbacks do not receive it.
Ordinary declaration/lifecycle machinery still owns its construction/migration/
destruction as fixed node storage, and the executor may reset or duplicate it when
that does not violate storage-lifecycle constraints because observable Tock-output
results cannot depend on its contents.

Source introspection publishes symmetric metadata for `State` and
`IndexedState`: a Clang nominal type identity (USR), a definition fingerprint,
size/alignment, and reflected field layout. That exact definition identity is the
cross-package-generation compatibility boundary for typed state migration. A
same-process type token remains sufficient when both generations use the exact
same loaded C++ type, but equal RTTI names or equal byte size alone are not a
safe hot-reload migration contract.

Fixed-size project-owned memory whose contents must cross an execution call
should use the same `NodeLayout` / `NodeStorage`, including for example:

- node `State` and `IndexedState`;
- history/latency/feedback carry;
- full fixed persistent sample/event buffers;
- root/compiler-owned activity state;
- bounded reusable background-evaluation workspaces; and
- other fixed-size compiler-selected project regions.

Dynamically sized tock/persisted output storage is an explicit exception because
its page/payload size follows actual coverage and retained authoritative content
rather than one fixed `NodeLayout`. Stable tock/persisted outputs live in an
executor-owned persisted-output store independent of one JIT generation; each
`CompiledGraph` supplies immutable endpoint bindings and representation facts.
`tock/ephemeral` outputs own no persisted result. Outputs without stable project
identity may use generation-local tock/persisted state where needed. This is not
a second node-state layout system: fixed `State`, optional tock-only `IndexedState`,
and compiler-known bounded regions still have one canonical `NodeStorage`, while
tock/persisted payloads and request-sized transaction storage are executor
sidecars. Tick/persisted finalized data can satisfy random-access reads directly;
replayable tick/ephemeral output can do so where upstream data is available. Explicit
recording bridges additionally use executor/runtime-owned slab-backed capture logs;
those blocks are temporary immutable transaction inputs and are reclaimed after the
background evaluation transaction that materializes them commits.

Invocation-local Tick-execution sample/event buffers occupy compile-time byte ranges in
the generated root stack. Sample and event ranges are lifetime-packed within
their payload class, then placed as two aligned subranges of one root allocation.
If the resulting byte count exceeds the configured limit, lowering re-runs
storage selection so eligible buffers use full `NodeStorage`; if the remaining
conversion/merge buffers still do not fit, compilation fails. Execution never
allocates a replacement dynamically on the audio-thread path.

This gives the whole-project compiler control over physical declaration order.
The current layout builder packs regions in declaration order while solving
`initialize_order` separately from dependency information, so lowering can
co-locate data in approximately the order generated O3 code will access it
without conflating physical locality with lifecycle ordering.

### No compiler-owned façade initialization path

Compiler-owned raw regions are bytes with semantic storage meaning, not a place
to persist C++ port façade objects. GraphJit should prefer immutable LLVM binding
records plus already-resolved stack or `NodeStorage` pointers over raw-region
initializers or pointer fixup passes. The generated root supplies the selected
concrete pointer to the imported node wrapper, which derives invocation-local API
views without branching on storage class.

If a future physical representation genuinely requires nontrivial persistent
runtime state, it should be modeled explicitly in the connection/storage plan and
given ordinary bounded storage/lifecycle semantics. Do not add a generic
first-call or pre-audio façade-construction mechanism merely because the old Graph
represented connections as mutable C++ objects.

### Compiler-owned raw regions

The authored `DeclarationContext::local_array()` API associates an allocation
with a typed `std::span` field in node `State`. Generated project code should not
be forced to manufacture a C++ state field merely to reserve a compiler-private
region whose address is known by constant offset.

`NodeLayoutBuilder` should therefore gain a low-level aligned-region declaration
primitive suitable for compiler-generated root storage. It should produce an
ordinary `NodeLayout::Region` in the same layout and allocation as every other
node region. Generated LLVM can then address the resulting storage by constant
offset after declaration/layout is complete, while `local_array()` remains the
typed authored convenience API.

This is an extension of `NodeLayout`, not a second storage system.

Truly request-sized caller input/output objects need not be embedded in
`NodeStorage`; their size may not be bounded at graph-compilation time. But if a
workspace has a known maximum size or is intentionally reusable across queries,
the compiler should prefer a root-owned `NodeLayout` region rather than a
separate project scratch allocation.

## Background evaluation is internal to the generated project

The normative port schema and execution semantics are in
[coverage_and_background_evaluation.md](./coverage_and_background_evaluation.md#13-authored-port-schema-retention-and-replayability).
`SequentialInputConfig` and `RandomAccessInputConfig` select consumer access;
`TickOutputConfig` and `TockOutputConfig` select producer callback; the output's
`OutputRetention` is separate. The checked-in source uses these final independent contracts directly; no
intermediate `Realtime*`/`Indexed*` port-config aliases remain.

An ordinary tick/ephemeral stream may feed a sequential input. It needs an explicit
recording-policy node only when it is **unreproducible** and a downstream input
requires random-access demand. Finalized persisted tick data and outputs of
contextually replayable `tick()` nodes can satisfy that demand directly. A tock
output (ephemeral or persisted) can feed sequential or random-access consumers;
a tock-to-random-access edge materializes addressable pages. Per-channel tiling
preserves each member's contract without implicit retention.

A separate replayability node type trait validates eligible `tick()`-only nodes:
no native `tick_block()`, no `State`, random-access inputs, history, or latency,
plus a fixed-version pure/deterministic replay contract. GraphJit reuses the
**existing** generated and LLVM-imported `tick_block()` wrapper in background
evaluation. Its static same-position temporal dependencies admit
compiler-synthesized F/R; upstream availability determines whether each particular
output can actually replay. The tock forward/reverse callback interface is unchanged.

`tock_coverage()` and its propagation callbacks **never run on the audio thread**.
Background workers prepare data. An audio-thread sequential input reads an existing
published page as-is even if stale, and substitutes that input's `neutral_value`
for a missing page. The audio thread never waits or recomputes a missing page.
A persisted output never evicts generated/finalized covered data for memory
pressure, age or invalidation. For page-backed outputs, recomputed pages replace old
published versions atomically. Coverage removal alone ends the retention obligation,
and old physical versions remain until reader pins are released.

The explicit recording bridge remains for unreproducible sequential sources.
`tick_block()` captures each produced recording block into provisioned slabs;
a background pass snapshots a fixed capture-sequence prefix and commits its
ordinary background evaluation transaction once. The page version advances on
commit, not capture insertion. Published retained pages never refer to the
recyclable capture slabs.

### Value specialization over immutable temporal data

GraphJit may later generate several code specializations inside one structural
compiled graph while all variants share the same `NodeLayout`, `NodeStorage`,
initialization, lifecycle, and persistent bindings. Switching specialization must
therefore require no state migration.

Candidate specialization values include settled controls, piecewise-constant
automation, small immutable tables, mode/bypass values, and other immutable
low-cardinality temporal data. A specialization is valid only for the temporal
regions/revisions that prove those values immutable; a generic path remains
available when no specialization applies.

Candidate buffers should be analyzed in lockstep over the defined project/render
domain. Compile only value tuples that actually occur rather than the Cartesian
product of each candidate's possible values. Candidate selection is globally
budgeted by compile time/code size and should prefer values that add useful LLVM
constants with few additional observed cases. Transition density/run length and
the size of the downstream optimization cone matter in addition to value
cardinality.

Independent specialization cones need not form whole-graph Cartesian products:
GraphJit may specialize separate regions independently when candidate values do
not interact until a cheap join. Very large immutable arrays should normally stay
in immutable external storage rather than being copied into every code variant;
small/high-value constants may be embedded directly.

Only code-only facts are eligible for this scheme. A value that changes
`NodeLayout`, state identity, initialization, lifecycle, or graph structure is a
structural recompilation input instead.

## Lowering boundary

The lowering result should carry immutable metadata sufficient for runtime work
without rediscovering project topology. In addition to ordinary Tick schedule
metadata it needs, as applicable:

- stable background-planning endpoint identities and generation-local ordinals;
- output production and retention per output, plus destination access/delivery facts per connection contribution;
- generated batched forward/reverse/tock traversal entrypoints and constant
  context-layout facts;
- background evaluation component/order information;
- per-node/per-port background evaluation transaction accumulator offsets/layout;
- semantic SCC IDs/validation products;
- persisted-output store bindings;
- persisted-output reverse-cut/page-validity binding facts;
- direct/transient plans for tock/ephemeral outputs;
- complete stored bindings for tock/persisted outputs;
- explicit recording-bridge output identities and capture metadata layout;
- immutable page-version bindings/publication metadata and reader-pin requirements;
- capture-sequence/frontier and allocator/reclamation integration facts; and
- event fan-in ordering/capacity facts.

Project sample rate is part of background-computed output semantics and must be supplied to
`tock_coverage()` and propagation where mappings depend on it.

## `CompiledGraph`

`CompiledGraph` remains the immutable JIT artifact: generated machine code plus
compiler metadata. The name describes compilation, not random-access semantics.

It owns no mutable dynamic persisted-output or transaction payloads. It describes
how a generation binds to executor-owned persisted-output storage, recording-capture resources, and
transaction workspaces.
Stable stored-output identity is derived from stable project node/member/output
identity, not generation-local primitive IDs. This applies to tick/persisted as
well as tock/persisted outputs; neither is a best-effort cache.

## `GraphExecutor` boundary

`GraphExecutor` owns the mutable runtime state for background evaluation and
persisted outputs:

- active/pending executable generations and canonical `NodeStorage`;
- optional tock-only `IndexedState` lifecycle/storage;
- stable persisted-output stores/immutable roots for `tock/persisted` outputs;
- candidate/published semantic versions plus immutable page versions;
- the recording-capture log, processed-sequence frontier, slab allocation/
  reclamation state, and page-version reader pins;
- reusable background evaluation transaction workspaces/accumulators;
- closed invalidation-root and demand-root normalization;
- exact batched forward invalidation and reverse-demand/tock transactions, with
  each node callback invoked at most once per applicable phase of a batch;
- complete `tock/persisted` candidate materialization;
- external random-access request/result/change-notification lifetimes; and
- safe whole-root-block executable/snapshot publication.

A semantic edit may build a complete candidate page version while audio-thread execution continues
against an older immutable published base. Playback reads those old pages as-is,
using each sequential input's neutral value only for genuinely absent pages. Recording captures may likewise
accumulate while a background evaluation pass runs, but they do not advance the page version.
The running pass keeps its fixed capture cutoff; a successor page version appears
only after the background evaluation transaction commits.

Stable executable replacement is rebinding rather than automatic semantic
invalidation. Unchanged tock/persisted payloads and compatible retained tick/
persisted content survive compatible JIT rebuilds. Persisted covered data is not evicted merely because it is stale or memory use grows. `tock/ephemeral` owns
no persisted output data to rebind.

Changing project sample rate invalidates/repropagates background-computed output semantics.
`tock/persisted` candidates are recomputed before publication; `tock/ephemeral`
naturally evaluates under the new rate. Existing tick/persisted samples are not automatically resampled or remapped to new sample indices: if preserving their
original timing matters, an explicit sampler/resampler node performs that DSP.

Receiving a new executable generation likewise does not mutate an in-progress live
pass. Expensive preparation happens off the hot path. Executable activation occurs
only at legal whole-pass boundaries. Recording captures published concurrently with
a background evaluation pass remain outside that pass's fixed capture-sequence cutoff and are
consumed by a later transaction.

## Failure semantics

A project mutation may successfully update desired/configured project state and
still fail during `GraphJit` compilation.

In that case:

- `ProjectGraph` retains the desired revision and compile diagnostics;
- `GraphExecutor` is not given the failed generation;
- the previous complete executable generation may continue running;
- a later project/definition change retries the entire root-build transaction.

Desired graph revision and active executable revision are therefore distinct
state even though compilation itself is synchronous.

## Compiler pipeline

`GraphJit` should orchestrate compiler phases, but deterministic graph analyses
and heuristics belong in ordinary testable compiler functions rather than in ORC
callbacks.

A target pipeline is:

```text
ConfiguredGraph
    |
    v
logical whole-graph lowering
    |
    v
dependency / schedule / SCC / region analysis
    |
    +--> background evaluation components + forward-invalidation/reverse-demand order analysis
    |
    v
history / latency / event-window analysis
    |
    v
connection implementation + liveness/reuse planning
    |
    v
canonical declaration/layout planning
    |
    +--> accepted native declare_node callbacks
    +--> compiler-owned raw regions
    +--> finalized NodeLayout + constant offsets
    |
    v
specialized project-root + background-evaluation-component LLVM generation
    |
    v
graph-specific optimization / -O3 / target optimization
    |
    v
ORC materialization
    |
    v
resolve generated root/component operations
    |
    v
CompiledGraph + finalized NodeLayout
```

Physical node state, persistent project state, and compiler-selected regions
whose contents cross calls all become one `NodeLayout`/`NodeStorage`.
Invocation-local representations instead use the statically packed generated-root
stack frame. Pure storage analyses may still decide which logical values need
either kind of region, their size/alignment, liveness, and desirable layout
before LLVM/declaration generation; they do not create a parallel persistent
allocation model.

See [sequential_port_storage_planning.md](./sequential_port_storage_planning.md) for
the rule that logical connections do not imply buffers, and
[coverage_and_background_evaluation.md](./coverage_and_background_evaluation.md) for exact coverage/change
propagation, whole-page covered-domain validity/versioning, reverse demand, and
batched tock semantics that lowering specializes.
