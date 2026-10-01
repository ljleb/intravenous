# Graph JIT Direction

_Status: current design direction for synchronous whole-project graph compilation._

[Batched Node Callback Direction](./batched_node_callbacks_direction.md) defines the planned normalized scalar/batch node callback API and the scheduling rules GraphJit will use to exploit it.


> **Planned random-access representation change:**
> [Random-Access Port Data, Audio Value Types, And Input Contract Direction](./random_access_port_data_direction.md)
> supersedes page-backed random-access sample storage and disjoint-Coverage assumptions
> in this document. Existing page-store discussion should be read as an implementation
> checkpoint until GraphJit/runtime lowering migrates to contiguous Region/Coverage
> sample views.

Related documents:

- [DSP Execution And Storage Glossary](./dsp_execution_storage_glossary.md)
- [project_graph_application_architecture.md](./project_graph_application_architecture.md)
- [sequential_port_storage_planning.md](./sequential_port_storage_planning.md)
- [coverage_and_background_evaluation.md](./coverage_and_background_evaluation.md)
- [realtime_background_execution_and_queues.md](./realtime_background_execution_and_queues.md)
- [builder_lowering_pipeline_design.md](./historical/builder_lowering_pipeline_design.md)
- [intravenous-llvm-hot-reload-and-whole-graph-design.md](./historical/intravenous-llvm-hot-reload-and-whole-graph-design.md)
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
`NodeInstances`, `GraphConnections`, and the `GraphExecutor` app module. Within
`GraphExecutor`, the internal `RealtimeExecutor` and `BackgroundExecutor` actors own
their respective runtime domains.

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
already-lowered concrete bundles and ports; GraphJit does not instantiate
them as executable runtime nodes.
Configuration pointer fields are reconstructed from symbolic retained-global
relocations: native pointer bytes are discarded during host planning, selected
immutable retained globals are deduplicated as package import roots, and final
node configuration globals contain LLVM-relocatable pointers plus byte addends
(or explicit null pointers). Each primitive invokes its exact accepted
native `declare_node` callback into the one canonical `NodeLayoutBuilder`.
The current lowering implementation still places both `State` and `TockState` in
the canonical `NodeStorage`. That is now a migration point rather than the target
contract: `NodeStorage` should contain the packed audio-thread realization, while
`TockState` moves to separately owned background-thread storage. Generated Tick
operations should address `State` and other realtime persistent regions by final
layout offset; generated background roots receive the separately managed
`TockState` for the implicated node.
Root execution now uses the connection-aware deterministic SCC/region schedule;
acyclic regions execute in dependency order and cyclic regions use slice-major
execution bounded by their derived feedback quantum. Callback imports are grouped
per package before a compile-local package module is consumed once, and repeated uses of one package
callback share one imported root while still receiving distinct node
configuration/storage contexts. The reflected compiler callback ABI uses
explicit pointer/count span records rather than assuming an
implementation-specific `std::span` object representation. The checked-in package
compiler currently retains one-node primitive Tick/skip/Tock/propagation anchors. The
planned batch API adds a normalized batch anchor beside every applicable scalar anchor;
GraphJit then chooses scalar or batch calls from dependency/cost facts without testing
whether the node author wrote `tick()`, `tick_block()`, or `tick_block_batch()`.

The generated Tick root exports only `tick_block`: it is the scheduler and may use
primitive `skip_block()`/`skip_block_batch()` callbacks internally when activity/skip
semantics make that legal. A separate Tick-root `skip_block` ABI would invert that
ownership and is intentionally absent. Graphs with background-evaluation work
additionally export off-thread forward, reverse, and evaluation batch roots. Those
roots use the retained `BackgroundEvaluationPlan` orders, imported reflected Tock
callbacks, and the existing imported `tick_block()` wrapper for replay; executor-owned
batch frames still supply coverage accumulators and page bindings. Multi-node primitive
callback batching is a separate planned layer inside those generated roots, as defined
in [Batched Node Callback Direction](./batched_node_callbacks_direction.md).
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
records consumed by the existing sample/event storage choosers, and
emits semantic transient/persistent/external storage and liveness requests.
A tock-produced output never becomes a same-slice sequential dependency merely
because a sequential consumer reads its materialized pages. The source now uses
`SequentialInputConfig`/`RandomAccessInputConfig` for consumer access,
`TickOutputConfig`/`TockOutputConfig` for production, and separate
`OutputRetention::{ephemeral,persisted}`. Connection analysis classifies those
axes independently per sample source channel and event source/target pair. It
retains Tick -> Sequential dependencies for Tick scheduling, records Tock ->
Sequential materialization and random-access materialization separately, treats
persisted Tick outputs as stored boundaries, and proves contextual replay for
eligible Tick/ephemeral paths by traversing sequential dependencies. The retained
`BackgroundEvaluationPlan` keeps semantic SCCs separate from the background evaluation DAG and
records authored Tock execution versus generated replay. An unreproducible
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
representations currently populate those slices from one storage base, but the
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
`choose_event_connection_storage_plan()` functions are the storage
policy boundary; GraphJit derives their requirement inputs and realizes their
returned choices rather than creating a competing policy layer. The old `Graph`
implementation is reference material only and must not constrain this runtime
representation. In particular, legacy fanout/cursor/storage objects should not be
carried forward merely to keep the old executor compiling.

The sample realization now has its own stable storage-plan layer in
`graph_jit/sample_storage_plan.{h,cpp}`. Primitive bindings refer to immutable
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
combined, and partially reused rather than leaving a historical whole-entry size
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

The storage planner consumes the storage decision already made by
`choose_sample_connection_storage_plan()`; it does not choose policy again.
Only Tick-to-Sequential branches receive sequential-storage representation handles
here; random-access and background-materialized branches remain unresolved for the later
background evaluation component executor. Whole-
port conversion is channel-granular: layout-only conversion and mono-to-stereo
duplication bind existing producer channels directly, while arithmetic conversion
materializes only its computed result channels. Semantic channel projection and
permutation preserve each source channel's storage producer identity and
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

### Planned value-type, port-size, pace, history, latency, and Coverage-ordering constraint analysis

The next port-generalization step should add graph-wide constraints over the static
port schemas before storage planning. `inputs()` and `outputs()` remain
`static constexpr`, so GraphJit/source introspection knows the exact port names,
each port's closed finite set of supported registered value types,
access/production contracts, and specialized context shapes.
A configured node instance may contribute constructor-dependent relationships through
one concrete callback:

```cpp
void constrain_ports(ConstrainPortsContext<MyNode>& ctx) const;
```

The initial continuous value registry contains scalar `Sample`, scalar `GlobalIndex`
and one-dimensional `FFTBlock`. `GlobalIndex` is the fractional global-sample
coordinate type; it retains the full integer global-index width plus 64 fractional
bits. Each registered type defines the rank and storage meaning of its `size()`;
`Sample` and `GlobalIndex` have no dynamic dimension while `FFTBlock::size()` is the
number of frequency values in one transported FFT block.

A port may statically declare either one registered value type or a finite set such as
`{Sample, GlobalIndex}`. The selected member is a graph-realization fact, not a runtime
per-value tag. Direct connections contribute equality between the source and target
value-type variables, so connectivity intersects/propagates their finite domains.
`constrain_ports()` may relate value types across a node in the same way:

```cpp
ctx.equal(
    ctx.input<"in">().value_type(),
    ctx.output<"out">().value_type());
```

An instance may also anchor one alternative from constructor/module configuration.
Empty intersections and still-ambiguous required value types reject the graph revision;
GraphJit does not silently select a preferred type. The initial design also performs no
implicit conversion between distinct registered value types: an explicit conversion or
DSP node is required when source and target types differ.

GraphJit must resolve both selected value type and every required type-dependent size
before transient-arena packing, alias selection, persistent-realtime placement, or
LLVM lowering.

`ConstrainPortsContext` exposes only constraint expressions, not solved realization
values. For ordinary two-sided constraints, assignment is shorthand for equality:

```cpp
ctx.output<"spectrum">().size() = configured_fft_size;
ctx.output<"out">().size() = ctx.input<"in">().size();
```

The left-hand side is a constraint proxy; this syntax adds an equality relation rather
than mutating a resolved value. The explicit `ctx.equal(...)` primitive remains
variadic for larger equivalence classes:

```cpp
ctx.equal(
    ctx.input<"a">().size(),
    ctx.input<"b">().size(),
    ctx.output<"x">().size(),
    ctx.output<"y">().size(),
    configured_fft_size);
```

Different constants in one local equality are an immediate configuration error. The
same applies when repeated assignment/equality constraints bind one local variable to
different constants.
Conflicts or unresolved variables that appear only after connection/value-type
constraints are joined reject the graph revision during compilation. Ordinary
spectral processors can therefore equate input/output FFT sizes and inherit one
upstream/downstream anchor instead of storing the FFT size in every node constructor.
`declare()` runs after resolution and may read concrete `ctx.input<...>().size()` /
`ctx.output<...>().size()` values when sizing `State` storage.

For callback code, single-type ports retain their ordinary concrete typed buffer API.
Only ports whose static schema lists more than one value type expose explicit
`is<T>()` / `as<T>()` accessors, and only for alternatives declared by that port. The
resolved value type is emitted as a static constant in the generated realization.
Authored branches that cover the finite alternatives can therefore be folded and the
unused paths removed by ordinary LLVM constant propagation/dead-code elimination.
Storage planning never allocates a runtime variant buffer: after the solve it sees the
selected concrete type's element size, alignment, rank and storage semantics.

The same constraint phase owns **port pace, Sequential-input history, Tick-output
latency, and Coverage ordering/sortedness**. None of these belongs in
`static constexpr inputs()/outputs()` once it can depend on constructor configuration
or another resolved port property. A Coverage output may, for example, preserve the
realized ordering of an input:

```cpp
ctx.output<"out">().ordering() = ctx.input<"in">().ordering();
```

The input ordering can itself depend on connected producers, so a downstream sorted
Coverage requirement may propagate transitively through several order-preserving
nodes. An intrinsically sorted producer or a sorted-input requirement anchors the
corresponding constraint. The solver/planner then determines where natural guarantees
and k-way descriptor merge suffice and where an explicit ordering realization is
required. The exact ordering lattice/proxy spelling may be refined with the solver,
but ordering participates in the same graph-wide realization pass as the other port
properties.

Pace is an exact
relative quantity of transported values per local logical node step; it is not
sample-rate conversion and
must not imply interpolation/filtering. Inputs and outputs may have different paces,
which represents FFT hop ratios and over/under-sampled graph regions without helper
nodes. Equality is again variadic, for example:

```cpp
ctx.equal(
    ctx.input<"a">().pace(),
    ctx.input<"b">().pace(),
    ctx.output<"out">().pace());
```

A rate-changing node may bind different ports to different exact quantities, for
example:

```cpp
ctx.input<"in">().pace() = 1;
ctx.output<"out">().pace() = 2;
```

or an STFT may bind its scalar input pace to a configured hop size and its FFT output
pace to one. Pace arithmetic/normalization should use exact integer/rational
relationships rather than floating-point rates so callback subdivision cannot
accumulate index drift.
Connections join those relations across the graph; the resolved result defines exact
global-index mappings and node execution-rate domains.

For a configured overlap FFT with transform length `N` and hop `H`, one ordinary
complete-window contract is:

```cpp
auto in = ctx.input<"audio">();
auto out = ctx.output<"spectrum">();

out.size() = N;
in.pace() = H;
out.pace() = 1;
in.history() = N - H;
out.latency() = 1; // for N=1024, H=512 and no zero-padded startup frame
```

The exact latency is part of the node's authored timing semantics, not an automatic FFT
formula. A node that intentionally treats pre-start history as freshly initialized
neutral/zero samples may emit an initial zero-padded frame and use zero output latency.
A node that promises only windows containing real input values must delay finalization
until enough input has arrived. In the `N=1024`, `H=512` case, the first invocation
consumes samples `[0,512)` but cannot finalize the first full-real-data FFT; the next
invocation sees `[0,512)` as history plus `[512,1024)` as the current block and may
revise/finalize the previous FFT output slot.

Because history and latency are solved realization properties, replacing a configured
node from an `on_message` handler may change them naturally. Replacing `FFT{1024,512}`
with another configured FFT causes size/pace/history/latency constraints to be solved
again, storage to be replanned, and the overlapping still-visible node-owned port state
to migrate under the ordinary graph-revision rules. No special history/latency mutation
path is required.

`ctx.sample_rate` is **not** available as a resolved number in `constrain_ports()`.
The node's effective sample rate depends on the very pace-domain solve being
constructed there. After resolution, declaration/initialization/execution contexts
may expose `ctx.sample_rate`, meaning the node's **effective local sample rate**. A
node running in a 2x oversampled region of a 48 kHz project must observe 96 kHz so
filters, oscillators, time constants, and other rate-dependent DSP remain correct.
This effective rate is distinct from per-port pace: an ordinary downstream filter may
have pace 1 on every port while its whole node executes in a 2x sample-rate domain.

Pacing and resampling remain separate operations. Pacing establishes how exact global
indices and transported-value counts correspond between rate domains. A resampler is
explicit DSP that computes new values. Future non-audio work may build on the same exact index-domain machinery, but
image/video value types and video-specific scheduling are out of scope for the
initial audio implementation.

A configured realization that defines pointwise `tick()` is legal only when the
relevant sequential Tick ports resolve to one compatible pace. This is a graph-
compilation check, not necessarily a source-introspection error: if a newly requested
revision resolves a `tick()` node to incompatible paces, compilation fails, the UI
receives the diagnostic, and the previous active revision remains untouched. A node
whose valid configurations can require heterogeneous paces should author adaptive
`tick_block()` instead.

For native `tick_block()`, block extent becomes a **per-port** resolved quantity.
The specialized context should expose, for example:

```cpp
auto in_frames  = ctx.input<"in">().block_size();
auto out_frames = ctx.output<"out">().block_size();
```

`block_size()` counts transported values participating in this invocation. It is not
the same as type-dependent `size()`: an FFT output may have `size() == 2048` and
`block_size() == 4`, meaning four FFT values, each containing 2048 frequency values.
GraphJit chooses a legal logical invocation quantum and derives every port's
`block_size()` from its resolved pace. An oversampler may consume `N` input values and
produce `2N`; an STFT may consume `512K` scalar samples and produce `K` FFT blocks.

A Sequential input with history also needs an explicit history-inclusive contiguous view
for algorithms such as FFTs. A provisional API name is `block_extended()`:

```cpp
auto current = ctx.input<"audio">().block();
auto analysis = ctx.input<"audio">().block_extended();
```

`block()` covers the newly consumed `block_size()` values. `block_extended()` covers
the immediately preceding resolved `history()` plus the current block. It remains a
Sequential-input API, not a Region/Coverage view. GraphJit should satisfy the contiguous
view by directly aliasing suitable producer/ring storage when possible and by
materializing a transient contiguous buffer only when the selected backing wraps or is
otherwise non-contiguous. Requesting history therefore does not imply a node-owned
`State` copy.

For delayed block-valued outputs, the generalized Tick-output facade should preserve the
existing `OutputPort::update()` semantic: a value within the resolved latency horizon may
be revised by a later invocation. The exact FFT-block API can be type-specialized, but a
complete-window FFT with one-block latency must be able to replace/finalize the
provisional output block authored by the preceding call.

The checked-in lowering currently assumes one common `block_size` for every
sequential sample port. That is an implementation checkpoint superseded by this
planned pace-aware contract.

### Tick/Sequential port realization rules

The remaining port work should preserve these invariants:

- **Storage contains data, not API facades.** Invocation-local sample/event
  data use the fixed generated-root stack frame. History/latency carry, full
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
- **Producer groups own storage representations.** Fanout consumers reference one
  producer-group representation or explicit derived branches; there is no default
  one-buffer/one-object-per-edge model.
- **Conversions and fanout materialization are explicit storage choices.** A
  producer writes its canonical source-layout representation once. Identity and
  aliasable converted/remapped branches bind its channels directly; arithmetic
  conversions use planned derived-result operations. Conversion is not hidden as
  mutable state inside `OutputPort`.
- **Transient and persistent state stay distinct in storage and semantically.**
  Current-block scratch is stack-frame storage and never migration state.
  `NodeStorage` holds either the exact carry crossing calls or an explicitly
  selected full persistent port buffer.
- **Direct is allowed to have backing.** With the authored node API, producer output
  still needs an addressable current-block representation. `direct` means no extra
  connection copy/materialization between producer and consumer. A later fusion/SSA
  optimization may eliminate even that backing where profitable.

The shell continues to use the generated-root and canonical realtime
`NodeLayout`/`NodeStorage` contract specified in this document: `CompiledGraph`
carries the finalized `NodeLayout` plus the generated root `tick_block`. Realtime
state lifecycle stays in `NodeStorage`; background-only lifecycle such as
`TockState` is owned separately by `BackgroundExecutor`. Primitive `skip_block` callbacks
are internal scheduling tools, not root operations. Whole-project lowering must not
reintroduce a second **realtime** node-storage layout or a synthetic project-wide
coverage/Tock ABI merely to expose internal Tock outputs.

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
  storage plan. Primitive-scoped operations run for every SCC slice;
  region-scoped operations run once at region entry or exit. This supports
  cyclic source history/latency, ingress and inter-region streams, split fanout
  scopes, same-SCC fan-in, converted/history-bearing feedback, and disconnected
  ports without execution planning inferring a window from buffer consumers.
- **Both kinds:** the root graph is required to have zero public/boundary ports.
  Device I/O and communication with other application modules enter through
  concrete node types, so there is no future root-boundary transport ABI to add.
  Random-access and background-materialized directions remain a separate lowering capability.

#### Tick/Sequential sample capability matrix

| Connection shape or feature | Current state | Storage behavior or remaining requirement |
| --- | --- | --- |
| One-source, exact-layout, zero-retention feed-forward | Implemented | Compatible consumers alias the producer representation directly. The producer still has addressable current-block backing, but the connection adds no copy. |
| Producer/consumer block-size mismatch | Implemented | The storage plan places the required block materialization before or after the relevant primitive while preserving absolute sample indices. A sliced producer can accumulate a root-call representation for unsliced or differently sliced consumers. |
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

| Connection shape or feature | Current state | Storage behavior or remaining requirement |
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
| Multi-producer fan-in touching a cyclic region | Implemented | Acyclic producers merge once after their completed invocation; each cyclic-region stage merges bounded producer-local streams after that region's final producer on every slice. A compiler-private source-index sidecar on the canonical aggregate preserves semantic equal-time ordering even when execution-region order differs from source order. Compact carry and persistent rings retain the indices with their events. |
| One derived materialization consumed both inside the source SCC and downstream | Implemented | Representation sharing is keyed by conversion and execution scope. The in-SCC and SCC-exit branches receive distinct scope-correct derived representations. |
| Mixed Tick/background or background-only event delivery | Planned; Tick lowering capability-gated | Per-source/per-target delivery is retained in the background plan. Playback/materialization belongs to background execution rather than another sequential-storage kind. |
| Unconnected primitive event port | Implemented in Tick lowering | Inputs receive a reset zero-capacity sequence. Outputs receive a bounded sink sized from `max_events_per_index`, history, latency, and root block size, with normal overflow telemetry. |

#### Remaining event-connection work

The remaining work should be treated as compatibility between semantic windows,
execution regions, and the existing storage representations—not as a request
for one universal event buffer.

Remaining semantic capability work:

1. Lower the already-planned mixed Tick/background and background-only event
   deliveries through the background executor and materialized-playback path.

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
  feedback operations before final storage realization: conversion output
  writes are counted once, full-persistent source reads and delayed-stream ring
  writes are explicit, compact feedback includes restore/commit copies, and
  identity fanout adds no copy. The remaining work is alias-versus-materialize
  comparison, weight calibration, and refining which eligible buffer is moved
  when several alternatives can satisfy the same limit. The fixed 64-event and
  16-KiB thresholds have been removed.

#### Event-connection implementation map

Use these files as the phase boundaries when extending the matrix:

- [`graph/realtime_port_planning.h`](../src/intravenous/graph/realtime_port_planning.h)
  contains the shared storage-kind vocabulary, data-specific requirement
  records, and pure policy choosers. It must not acquire topology-specific
  lowering logic.
- [`graph_jit/connection_plan.h`](../src/intravenous/graph_jit/connection_plan.h)
  and [`connection_plan.cpp`](../src/intravenous/graph_jit/connection_plan.cpp)
  derive logical event connections, producer groups, SCC schedule facts,
  retention requirements, and implementation choices before storage lowering.
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
  node API contract, not the storage-policy layer.

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
`do_tick_block()` as one-sample context per frame, advancing input/output
cursors after every call. Primitive maximum-block slicing reconstructs the same
facades at each slice index, so authored-latency revision must remain valid across
both slice and root-call boundaries. The checked-in implementation currently publishes exactly one sample frame per output
per `tick()`, or one common `block_size` per output per `tick_block()`. The planned
pace-aware contract above supersedes that equal-block assumption: pointwise `tick()`
is valid only for a realization whose relevant ports resolve to equal pace, while
native `tick_block()` consumes/produces each port's resolved per-port `block_size()`.
Release execution still need not maintain a redundant dynamic production-count check
once GraphJit has proven and lowered those counts.

`skip_block()` uses the same block anchoring. A custom skip callback owns its own
output semantics; when one is absent the generic helper generates silence for
every channel of every sample output and advances inputs by the skipped block.
GraphJit's generated root does not yet schedule primitive skips, so this remains a
generic callback contract until activity/TTL lowering lands.

The planned batch callback layer preserves these exact scalar semantics while exposing
normalized `do_tick_block_batch()` and `do_skip_block_batch()` operations. `tick()`
generates a sample-major/lane-minor batch loop to expose cross-instance SIMD; native
`tick_block()` and scalar skip callbacks generate an ordinary lane loop; a node that
authors only a native batch form receives scalar behavior through a one-lane batch
facade. Both compiler anchors remain available, so GraphJit may cluster compatible
ready nodes for SIMD/instruction locality or fall back to scalar execution when a
heterogeneous dependency schedule makes batching unprofitable. Authored callback-shape
checks remain entirely inside the node trait/helper API. See
[Batched Node Callback Direction](./batched_node_callbacks_direction.md).

Fresh compiler-owned persistent connection state is lifecycle-owned. Sample
feedback/carry/alignment raw regions and persistent event representations install
`NodeLayout` raw initializers; exact-shape persistent regions skip initialization
when migration restores their bytes. The generated Tick root contains no
first-call initialization guard. Transient event sequences and feedback cursors
are fixed stack/SSA state for one root call and reset there. Unequal-latency
sample-feedback alignment uses the initialized alignment-ring samples directly as
branch prehistory. As real source frames arrive they overwrite those entries
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
   byte addends, and explicit null entries rather than native process addresses.
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
   storage plan with immutable representation handles, canonical producer
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
   whole-project LLVM using absolute-position-addressed storage and contains no
   runtime converter object, heap allocation, or `OutputPort` conversion state.
9. **Sample history and latency.** **Landed for declared sample history/latency in Tick execution.**
   `compact_persistent_carry` uses one transient absolute-position-addressed working ring plus
   exactly the retained tail in persistent raw `NodeStorage`; the tail is restored
   before its producer and committed after producer-side materializations. Larger
   retention uses `persistent_ring`, binding primitives directly to one power-of-two
   persistent ring. Immutable input bindings carry authored history/read latency;
   aliasable converted branches read retained producer history directly, while
   arithmetic derived results materialize the historical window they actually need.
   Both modes use absolute sample-index addressing. The **currently landed** generation
   migration for compiler-owned raw regions is only an exact-shape copy keyed by the
   current storage plan; transient arenas never migrate. That implementation is not
   the final semantic contract for declared output latency or input/output history.
   Before optimization work proceeds, graph-revision reconciliation must preserve those
   port-visible windows as concrete-node-owned state even when the old/new storage
   representation, source connection, fan-in set, or retained size changes. Feed-forward whole-graph
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
    source-index sidecar lets later stages insert equal-time events at their
    semantic source position while authored ports continue to see an ordinary
    contiguous `TimedEvent` sequence.
    Implicit conversions are intentionally non-expanding: one source event may
    produce zero or one target event, never generate additional events.
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
    retained representation capacity as input to storage-plan comparison. The
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
    while Tock materialization/materialization and persisted Tick boundaries are retained
    as background facts. Unreproducible Tick/ephemeral -> RandomAccess demand is the
    remaining connection-level rejection and requires explicit recording.
16. **Landed checkpoint: replayability trait and contextual replay planning.** The
    checked-in trait currently opts in an existing `tick()`-only node with no `State`,
    no Random Access inputs, history or latency and a fixed-version pure/deterministic
    contract. GraphJit retains the generated `tick_block()` import, proves upstream
    availability through ordinary Sequential dependencies, stops at persisted
    boundaries, rejects replay cycles and records generated pointwise F/R plus
    background replay ordering. The target contract removes only the Random Access
    prohibition: such inputs become ordinary prepared replay dependencies, with
    authored conservative F/R where their address mapping is not mechanically
    pointwise. Contextual replayability remains a per-path compiler fact, not an
    output config field.
17. **In progress: subset-based storage inference and ordinary background evaluation.**
    Exact source/target port-atom incidence partitioning, capability joins, and
    immutable storage planning have landed. Source representations now
    select canonical pages/current-Tick views/materialized or transaction-local placement;
    direct views stay copy-free, while derived conversion/fan-in templates share only
    under an exact compile-time key, with materialized addressable results subsuming
    otherwise-identical materialized sequential results. The current monolithic executor substrate still owns active/pending `CompiledGraph` +
    `NodeStorage` realizations pending the internal decomposition of the `GraphExecutor`
    app module into `RealtimeExecutor` and `BackgroundExecutor` actors,
    stages pending storage without sampling live state, performs migration at explicit
    quiescent-boundary activation, and keeps activation out of its Tick entry point.
    The executor now also realizes compiler-planned background-evaluation accumulator records as a
    reusable `BackgroundEvaluationCall`, runs exact transactional forward/reverse propagation
    through the generated roots, accumulates fan-in/fan-out before the one-call-per-node
    callbacks. Committed semantic coverage is now separate from the standalone,
    executor-independent reusable propagation workspace: F/R produces a move-only
    prepared result exposing exact changes/requirements and immutable per-node
    activity. Reverse-demanded nodes receive `evaluate` only after both traversals
    succeed. No public executor operation commits the candidate after F/R alone.
    `BackgroundEvaluationTransaction` retains that prepared result and promotes it
    only at final transaction commit, together with page publication when a candidate
    is present.

    Immutable before/after-node materialization placement, compact runtime binding
    slots and replay schedule constraints/slots now live in the compiled background
    plan and are validated without LLVM. The executor-level canonical sample/event
    page store now owns one immutable published root, private structurally shared
    candidates, semantic/page versions, stale-base rejection, pre-registered
    non-owning reader pins and deferred non-audio reclamation. Address-stable runtime
    sample/event views and sparse transaction-owned backing now interpret each
    `PortStoragePlan` index, bind non-owning published-page readers, preserve the
    stable/generation-local identity boundary and validate required views before the
    realization is sealed. The realization now executes compiler-retained operations
    exactly once: direct delivery stays copy-free, sample operations apply planned
    latency/conversion/projection assembly, and event operations reuse the common
    conversion plus stable semantic-order k-way merge primitives. The generated
    evaluate root now invokes transaction-supplied prepare/finalize hooks immediately
    around each active authored-Tock invocation or complete replay loop. Each hook
    receives only an opaque `BackgroundStorageOperationFrame`; that frame retains the
    node's immutable before/after operation spans and executes them in compiler order.
    Forward/reverse propagation remains hook-free, and generated code receives no
    executor, store or transaction pointer. The transaction-local
    `BackgroundEvaluationCallFrame` now owns those operation frames, binds the dense
    runtime slots to sealed-storage Tock facades, and reserves the compiler-sized
    reflected sample/event arrays for replay. Logical binding coverage is supplied
    separately from storage coverage so direct read latency and output fan-out remain
    explicit. Replay regions are accepted only as an ordered, nonempty schedule whose
    entries satisfy the compiled maximum block size. The call-frame owner now derives
    isolated power-of-two raw sample/event buffers from that schedule, fills replay
    inputs after the placed prepare operations, flushes outputs before the placed
    finalize operations, and binds an empty replay state rather than live realtime
    state. The complete transaction pins one page snapshot, completes persisted-page
    demands between forward and reverse propagation, invokes the generated root with
    prepared activity and privately stages produced or invalidated pages only when
    page state changes. Page-free operations do not advance the page version, but
    revalidate their pinned base before coverage promotion; stale publication is
    rejected. Selected pinned inputs are sealed only when compatible published pages
    contain their complete requested domains. Before any publication, the transaction rejects incomplete selected
    sample/channel production and any callback write that missed its selected storage
    or exceeded an event bound. Multi-output replay flushes only each output's selected
    coverage from the node-wide union schedule. Semantic coverage is promoted only
    after any required page publication succeeds. In the target split,
    `BackgroundExecutor` exposes only that end-to-end operation. The Tick root ABI receives one narrow
    callback-scoped invocation record containing only resolved sequential and Random
    Access views. `RealtimeExecutor` pre-registers its page-reader slot off the realtime
    thread, pins one published root for the complete generated-root call, and keeps
    that pin in a non-copyable `TickInvocationFrame`; neither the frame owner nor the
    store crosses the generated ABI. Immutable per-node dynamic Sequential and Random
    Access sample/event slot ranges have now landed. Random Access lowering is active,
    and Sequential lowering now overlays compact dynamic slots onto each imported
    primitive's complete stack-local input-binding array so unaffected and mixed live
    inputs remain intact. Sequential sample callback binding is active: the
    realization-owned workspace preallocates one bounded playback ring per external
    sample slot, copies the callback window plus authored history from the coherent
    materialization/page root, and preserves each logical input's neutral value for
    missing frames. Sequential event callback binding is active as well: each slot
    owns a power-of-two bounded sequence sized from its selected aggregate event rate
    and the specialization's maximum block size, then copies only the callback's
    absolute-time window; missing data is the empty sequence. The realization-owned workspace
    resolves persisted identities and allocates its address-stable view arrays off the
    audio thread; published roots retain precomputed exact output coverage, so Random
    Access callback binding only retargets direct persisted-page views to the one
    pinned snapshot.
    Background transactions now freeze compiler-routed derived/fan-in/converted and
    ephemeral Random Access results, together with direct background-only Sequential
    materializations, into an immutable `TickMaterializationSnapshot`. Mixed
    `current_tick` composites remain generated-root work because they also consume live
    Tick sources. The snapshot is promoted through a separate pre-registered-reader store. Promotion is a
    no-fail owner relink after any page publication; retired owners are reclaimed
    explicitly off the audio thread. `TickInvocationFrame` pins both roots for the
    callback, and materialized bindings are accepted only when the snapshot's
    generation and recorded page version match the active realization and pinned page
    root. Retained Sequential sample and event slots are now both supplied through
    bounded callback binding without exposing either snapshot owner.
    Final commit
    advances prepared coverage, page publication and release of the selected
    producer-queue prefixes together; every failure advances none. Background ephemeral Random Access may use
    transaction-local page-backed materialization; Tick-time ephemeral Random Access
    must be materialized before the callback. Playback never blocks, reclaims retired
    storage or invokes Tock.
18. **Tick/persisted recording transport and disposition semantics landed.**
    Realtime-produced persistence uses producer reserves and background pending
    queues. The old capture store, capture-output registry, sequence/frontier log and
    `TickInvocationWorkspace` compatibility adapter have been deleted. The runtime uses
    `RealtimeExecutor` for realtime
    generation/storage and producer endpoints, `BackgroundExecutor` for the
    worker/background evaluation/persisted-state owner, and non-app-module
    `AsyncCapacityManager` infrastructure for producer-specific blocks.

    Each realtime producer advertises `C/L/H`; capacity is provisioned ahead of demand
    in power-of-two blocks and allocator slab granularity remains manager-internal. The producer builds complete block chains privately,
    writing sample/event data directly into them where layout permits, then publishes
    the chain at a realtime pass boundary. The generated ABI receives only compact
    producer/binding operations; no executor, queue owner or capacity manager enters
    generated code.

    The internal `RealtimeExecutor -> BackgroundExecutor` handoff connects
    each already initialized incoming chain to that producer's SPSC queue with one
    cheap publication/pointer operation. It does not synchronously run background
    evaluation. `BackgroundExecutor` independently pins one finite `(first,last)`
    prefix from each relevant producer queue when selecting a workload. There is no
    atomic cross-queue snapshot; later arrivals are later work. If cross-queue atomic
    visibility is ever required, stop and design that feature explicitly.

    Preserve fixed recorder semantics: `untouched` publishes nothing and preserves the
    prior RAM recording; ordinary writes overwrite the addressed range; `write_void()`
    publishes an explicit erase. An explicitly authored zero-event Tick/persisted
    block remains ordinary authoritative empty event data. Resource exhaustion is a recording/persistence
    failure and never permission to drop a written block.

    A Tick/persisted output is the recording output; there is no separate recorder
    port kind, retention mode or runtime policy. An explicit recorder is an ordinary
    authored node that consumes live Sequential input and writes Tick/persisted output.
    Invocation-local `OutputDisposition` is implemented only by the statically
    specialized Tick/persisted sample/event accessors and carried by their reflected
    output bindings. Ephemeral Tick and Tock output writes perform no disposition
    update. Reflected invocation-binding construction resets the state to `untouched`,
    while repeated accessor construction within that invocation preserves it. An
    ordinary write (including an explicit empty event-block write) marks it `written`,
    and `write_void()` marks it `voided`; the generated post-step operation can
    therefore observe authored state without a recorder-specific node API.
    The low-level generic sample/event port objects carry no disposition pointer or
    state. Tick disposition covers the entire addressed callback block (one sample for
    scalar `tick()`); output history/latency does not widen the recorded range. Tock
    continues to author exact arbitrary coverage.

    Background commit applies exactly the selected queue prefixes, publishes a
    coherent immutable persisted-state/page version when appropriate, and then releases
    completed prefixes through non-realtime reclamation. The published version reaches
    `RealtimeExecutor` as an immutable pointer and becomes active only at a legal
    realtime pass boundary.

    Remaining recording work stays on the existing Tick/persisted transport. It must
    not reintroduce a recording-specific store or an alternate
    `TickInvocationWorkspace` construction path.
19. **Generation reconciliation.** Rebind compatible stable persisted stores across
    generations. Persisted generated/finalized data remains retained throughout its
    covered lifetime; coverage removal is the only semantic deletion condition.
    Superseded storage versions are reclaimed after their readers release them.
20. **Implement concrete-node port-state continuity and transition realizations.**
    Treat each surviving input history and output history/latency window as though it
    were private state owned by that concrete node/port, regardless of how the steady
    storage planner aliases or shares it. Carry stable node/virtual-member/port/channel
    identities plus realization metadata across generations, preserve the overlapping
    valid temporal range, and materialize temporary transition state when the new
    steady representation cannot itself expose inherited values. `RealtimeExecutor`
    activates the transition realization at the graph splice and, when its finite
    inherited-state horizon expires, switches at a safe root boundary to the already
    compiled steady realization. This correctness stage is mandatory before storage or
    code optimization work.
21. **Finish remaining authored-node semantics** (nested declarations, compiler-
    owned regions, activity/TTL, detach, events and skip scheduling).
22. **Add first-class primitive callback batching.** Add the typed batch ranges and
    trait-normalized scalar/batch helper pairs for Tick, skip, Tock and forward/reverse
    propagation; retain both compiler anchors; classify compatible resolved
    realizations; and let Tick/background schedulers group ready same-class nodes while
    retaining scalar fallback. Use virtual-node member grouping as a hint but discover
    batching opportunities over the flattened concrete graph. See
    [Batched Node Callback Direction](./batched_node_callbacks_direction.md).
23. **Optimize** with measurement-driven batch sizing, cross-node SIMD, loop fusion,
    materialization placement, storage liveness and bounded immutable-value
    specialization only after state continuity and the remaining correctness semantics
    are established.

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

Old and new generations must be able to coexist while `RealtimeExecutor` finishes a
pass, materializes `NodeStorage` migration, or retains active/pending generations.
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

The optimized root keeps ordinary node semantics, but declaration is consumed as a
compile-time layout contract rather than materialized as a runtime JIT entrypoint.
Runtime behavior is therefore:

```text
compile time: declare/layout planning
runtime:      NodeStorage initialize()/move()/release()
runtime:      generated tick_block() (owns child tick/skip decisions)
```

The root remains zero-port at the project graph boundary, but its native Tick ABI
also receives one narrow invocation frame for dynamic background-derived bindings.
That frame contains only compact immutable views selected for this callback: pinned
published-page inputs and any `TickMaterializationSnapshot` bindings. It contains no
an executor pointer, page-store pointer, transaction object, or storage-discovery API.
GraphJit retains the compile-time node-port-to-slot mapping; the executor fills the
slots before entering the generated root.

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

## One canonical fixed-layout realtime `NodeLayout` and `NodeStorage` model

There is exactly one **fixed-layout realtime** storage model for each executable
realization: the existing `NodeLayout`/`NodeStorage` machinery. A logical graph
revision may temporarily have both a transition and a steady executable realization,
but each realization uses this same canonical audio-thread storage model rather than
introducing a second graph-kernel state arena.

`NodeStorage` is intentionally narrower than "all persistent runtime memory". Its
job is to pack the structures needed by generated audio-thread execution as tightly
and access-locally as practical: authored `State`, compiler-selected cross-call
sample/event carry, feedback/activity state, and other fixed regions the audio root
actually needs. Request-sized objects, dynamically growing background data,
persisted-output pages, and `TockState` are not part of this packed realtime arena.

`GraphJit` must not introduce `CompiledGraphNodeStorageLayout`,
`GraphKernelStorage`, or another parallel fixed **audio-thread** state arena.
Lowering populates one `NodeLayoutBuilder` per executable realization and finalizes
it before emitting realtime storage accesses into LLVM. Each completed `NodeLayout`
becomes part of its `CompiledGraph`, and `RealtimeExecutor` creates/owns the
corresponding `NodeStorage` while that realization can be active or is needed for a
handoff.

### `TockState` is background-owned sidecar state

`TockState` is Tock/background acceleration state, not realtime node storage. It is
managed separately from `NodeStorage` and is never addressed by the audio-thread
root. Ordinary dynamic allocation is allowed inside it. Its internal size may
therefore follow actual background requirements such as input coverage/content
without changing `NodeLayout` or forcing a realtime-storage migration.

A node may use `initialize()` to set up its instance's `TockState` from configuration
and resources. Any derived data that depends on current input contents/coverage is
computed or recomputed by the appropriate background `tock*()` work instead.
`TockState` remains non-semantic acceleration state: outputs must be correct from a
freshly initialized instance, and retaining/moving compatible acceleration state
across realization changes is only an optimization.

Source introspection may still publish nominal/definition metadata for `TockState`
for lifecycle/hot-reload validation, but `TockState` field layout is not a
`NodeLayout`/`NodeStorage` placement contract. `State` remains the authored typed
state whose realtime storage relationship is fixed by declaration/layout.

Fixed-size project-owned memory whose contents must cross **audio-thread root calls**
should use the same realtime `NodeLayout` / `NodeStorage`, including for example:

- authored node `State`;
- history/latency/feedback carry selected for persistent placement;
- full fixed persistent sample/event buffers selected for realtime access;
- root/compiler-owned realtime activity state; and
- other fixed-size compiler-selected regions touched by the audio root.

Background-owned memory is a sidecar even when it happens to have a fixed size.
Examples include `TockState`, reusable background-evaluation workspaces, transaction
frames, and dynamically sized persisted-output storage. Persisted output continues
to use executor-owned stable page stores because its size follows semantic coverage
and retained authoritative content rather than one realtime `NodeLayout`.

Tick/persisted finalized data satisfies Random Access through the published
persisted-page snapshot; replayable Tick/ephemeral output can satisfy it through
background replay where upstream data is available. Tick/persisted recording uses
executor/runtime-owned slab-backed producer reserves and pending queues. Pending queue
blocks are not a second Random Access source: newly recorded Tick/persisted data
becomes visible only after publication into the canonical page store.

Invocation-local Tick-execution sample/event buffers occupy compile-time byte ranges
in the generated root stack. Sample and event ranges are lifetime-packed within
their data class, then placed as two aligned subranges of one root allocation. If
the resulting byte count exceeds the configured limit, lowering re-runs storage
selection so eligible **realtime** buffers use full `NodeStorage`; if the remaining
conversion/merge buffers still do not fit, compilation fails. Execution never
allocates a replacement dynamically on the audio-thread path.

This gives the whole-project compiler control over realtime storage declaration
order. The current layout builder packs regions in declaration order while solving
`initialize_order` separately from dependency information, so lowering can co-locate
data in approximately the order generated O3 audio code will access it without
conflating storage locality with lifecycle ordering. `TockState` placement is not
part of that optimization problem.

### No compiler-owned façade initialization path

Compiler-owned raw regions are bytes with semantic storage meaning, not a place
to persist C++ port façade objects. GraphJit should prefer immutable LLVM binding
records plus already-resolved stack or `NodeStorage` pointers over raw-region
initializers or pointer fixup passes. The generated root supplies the selected
concrete pointer to the imported node wrapper, which derives invocation-local API
views without branching on storage class.

If a future storage representation genuinely requires nontrivial persistent
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

Truly request-sized caller input/output objects are not embedded in `NodeStorage`;
their size may not be bounded at graph-compilation time. Background workspaces also
remain outside `NodeStorage` even when bounded/reusable: fixed size alone is not a
reason to pollute the packed audio-thread arena. Realtime compiler-owned workspaces
that genuinely survive audio-root calls are different and may use ordinary
`NodeLayout` regions.

## Port history and latency are node-owned semantic state

Port history and latency are configured/resolved **effect requirements**, not declarations that a
particular connection buffer must exist. Their graph-revision semantics must be
observationally equivalent to a simple conceptual implementation in which each
concrete node privately owns its port-related state alongside its authored `State`:

```text
concrete node
    +-- authored State
    +-- each Sequential input's resolved history
    +-- each Tick output's resolved history
    `-- each Tick output's resolved latency/future window
```

The storage planner remains free to alias several of those conceptual states onto one
producer timeline, a compact carry, a full ring, a derived fan-in result, or another
representation. Steady execution should continue to minimize copies. **Semantic
ownership does not imply one copy.** The ownership rule exists so graph
replacement has a representation-independent answer about which values must survive.

For example, if a graph switches at absolute position `P` from:

```text
A.out -> C.in(history = 64)
```

to:

```text
B.out -> C.in(history = 64)
```

then immediately after activation `C.in[P-64, P)` remains the resolved history that C
actually observed before the splice, while positions from `P` onward use the new
connection. Rewiring does not reinterpret C's past as B data. The same rule applies to
fan-in: if C previously observed an `A+B` composition and the new graph supplies
`A+D`, its still-visible history remains the old resolved `A+B` values until they age
out.

Likewise, a surviving output owns its resolved history and latency/future state.
Changing consumers must not discard that state. When a resolved history/latency
extent itself changes, migration preserves the intersection of the old valid semantic
range with the new required range; newly exposed range receives the normal fresh-state
initialization semantics, and no-longer-observable range may be discarded. storage
ring capacity, compact/full representation choice, root block size, fanout count, and
connection incidence are not semantic identities.

Compiler-managed port-state identity must therefore be rooted in the stable concrete
node path already present in configured/project graph metadata: user-instantiated
leaf/module identity, virtual-node/direct-member path, port direction and index,
channel index (or event stream), plus a small state-role discriminator such as
`input_history`, `output_history`, or `output_latency`. Ordinary connection identity,
allocation number, incidence-atom index, buffer kind, capacity, and byte offset are
not part of that identity. A connection may disappear or change while the destination
input state survives.

Each compiled realization must carry cold metadata that explains how those semantic
state pieces are represented in that realization. A state piece may be a contiguous
region, a ring range, a channel view into a shared producer representation, or a
resolved/composed view that has no dedicated steady-state buffer. Any optimization
that aliases or eliminates a conceptual private port-state buffer must still emit
enough realization metadata to recover the semantic window during a later graph
replacement. The realtime kernel never consults the identity map. `RealtimeExecutor`
uses it only when reconciling executable realizations, and may copy/materialize the
same underlying stored data more than once if several node-owned semantic states
previously shared it.

### Transition and steady realizations of one graph revision

A new logical graph revision may require a temporary compiled realization solely to
preserve inherited node-owned port state. This happens when the steady storage
representation cannot itself express the old values. The compiler may therefore
produce up to two executable realizations for one logical revision:

```text
old revision G0
      |
      | splice at P
      v
G1 transition realization
      |
      | inherited transition-only state expires
      v
G1 steady realization
```

For the rewiring example above, steady G1 may optimally let `C.in` read B's producer
storage directly. The transition realization may additionally own a 64-sample carry
containing C's pre-splice resolved input, or it may temporarily pin/read suitable
old-generation storage when that is cheaper and semantically exact. As new B-derived
samples arrive the inherited range ages out. Once no transition-only state remains
observable, it is legal to activate the steady realization that contains no such
carry/old-generation dependency.

This is not a second semantic graph edit and should not require a later compilation.
GraphJit has the old realization, the new logical graph, the splice position, and all
finite history/latency windows during the original rebuild, so it can pre-plan and
compile both realizations together when necessary. The handoff horizon is defined in
absolute timeline positions and activation occurs at the first legal root callback
boundary at or beyond the last inherited state's expiry. A fixed-block implementation
may precompute an equivalent block count, but callback count is not the semantic
coordinate.

Two realizations are not mandatory. If the G1 steady representation can directly
receive every surviving state piece, `RealtimeExecutor` migrates into it and activates it
without a transition realization. Conversely, state that remains observable
indefinitely is ordinary G1 state, not transition-only state, and must be represented
by the steady realization. At the later transition-to-steady handoff, authored `State`
and any other still-live node/port state have evolved under G1 and must be reconciled
from the transition realization into the steady one; only the transition-only inherited
portion is known to have expired.

If another project revision arrives before the current transition expires, that new
revision reconciles from the **currently active transition realization** and its
semantic state views. Any pending steady realization for the superseded revision may
be discarded. Rebuild logic must never reconstruct history from an older logical graph
or assume that the not-yet-activated steady plan describes the state the node has
actually observed.

This continuity work is a **correctness prerequisite for optimization**. The eventual
NodeStorage cleanup is a separate task. The first implementation may retain existing
compiler-owned raw-region and `NodeStorage` special cases internally, provided their
migration/reconciliation behavior implements the node-owned semantic rule above.
Only after this rule is tested across rewiring, fan-in/fanout changes, size changes,
and representation changes should GraphJit invest further in eliminating/aliasing
state or other storage/code optimizations.

## Background evaluation is internal to the generated project

The normative port schema and execution semantics are in
[coverage_and_background_evaluation.md](./coverage_and_background_evaluation.md#13-authored-port-schema-retention-and-replayability).
`SequentialInputConfig` and `RandomAccessInputConfig` select consumer access;
`TickOutputConfig` and `TockOutputConfig` select producer callback; the output's
`OutputRetention` is separate. The checked-in source uses these final independent contracts directly.

An ordinary Tick/ephemeral stream may feed a Sequential input. It needs authored
persistence or an explicit recorder only when it is **unreproducible** and downstream
Random Access demand reaches it. Tick/persisted data satisfies that demand through
the canonical published persisted-page snapshot. Contextually replayable
Tick/ephemeral and Tock/ephemeral data use immutable addressable materialization;
background-only consumers may use transaction-local storage while Tick-time consumers
require materialized data. Tock/persisted uses the same canonical page read path as
Tick/persisted. Per-channel tiling preserves each member's contract without implicit
retention.

The checked-in replayability node type trait is still an implementation checkpoint:
it accepts eligible `tick()`-only nodes with no native `tick_block()`, no `State`, no
Random Access inputs, no history/latency, plus a fixed-version pure/deterministic replay
contract. The target semantic rule removes the Random Access prohibition. A Random
Access input is a declared replay dependency that background planning must satisfy
before invoking the Tick wrapper; it is not hidden state and does not by itself make
the computation unreplayable.

GraphJit reuses the **existing** generated and LLVM-imported `tick_block()` wrapper in
background evaluation. Mechanically same-position dependencies admit compiler-generated
F/R. A replayable Tick node with non-pointwise Random Access addressing participates in
the same background forward/reverse dependency framework as authored Tock planning;
its callbacks may conservatively request/invalidate complete potentially relevant
coverage when a tighter value-blind bound is unavailable. Upstream availability then
determines whether each particular output can actually replay.

The planned batch normalization removes the callback-spelling restriction from the
long-term replay model. Replay eligibility should be stated against normalized
`do_tick_block()` semantics, so an otherwise eligible native `tick_block()` can replay
at legal invocation quanta and a `tick_block_batch()`-only node can replay through its
one-lane scalar adapter. `tick()` remains valuable because it exposes stronger
pointwise structure for automatic cross-node vectorization. The detailed migration is
in [Batched Node Callback Direction](./batched_node_callbacks_direction.md).

`tock_coverage()` and its propagation callbacks **never run on the audio thread**.
Background workers materialize data. An audio-thread sequential input reads an existing
published page as-is even if stale, and substitutes that input's `neutral_value`
for a missing page. The audio thread never waits or recomputes a missing page.
A persisted output never evicts generated/finalized covered data for memory
pressure, age or invalidation. All persisted outputs use the canonical persisted-page
store; recomputed/finalized pages replace old published versions atomically. Coverage
removal alone ends the retention obligation, and old storage versions remain until
reader pins are released.

The same preparation rule applies to Tick nodes with Random Access inputs, including
replayable ones. Before the generated root enters an audio-thread `tick_block()`
invocation, every Random Access port is already bound to its callback-pinned persisted
or `TickMaterializationSnapshot` Region/Coverage view. Tick execution performs no Tock
work, dependency discovery, request-driven materialization, or waiting. During
background replay, the transaction may recursively run required Tock/replay work, but
it completes and installs the planned immutable Random Access views **before** invoking
the imported Tick wrapper.

Storage selection is performed over **overlapping port subsets**, not
one connection at a time. GraphJit partitions source/target channel incidence into
port atoms, joins the independent capabilities required by every fan-out/fan-in
use, then deduplicates conversions/compositions and coalesces equivalent
atoms. This allows one Tock/ephemeral subset with Tick-time Random Access to require
an addressable materialized window without promoting unrelated channels, while a
Tick/persisted subset can simultaneously retain current Tick storage for Sequential
consumers and canonical pages for Random Access.

The preliminary Tick-time Random Access path pins a published page snapshot at the
root callback boundary. Newly queued Tick/persisted data from that callback is not
visible until persisted-state/page publication and a later snapshot selection. A
same-Tick recent-data overlay is a later optional optimization, not baseline semantics.

An explicit recording node for an unreproducible Sequential source authors an ordinary
Tick/persisted output; it does not introduce another port or transport category.
Tick/persisted outputs use producer-specific reserve/pending-queue SPSC transports. A background pass
independently pins one finite prefix per relevant producer queue and commits its
ordinary background transaction once. The page version advances on commit, not queue
insertion. A page candidate may copy or adopt compatible queue payload data, but
consumers see the canonical persisted-page/recording abstraction rather than a
separate queue-storage read path.

Background evaluation keeps one generated statically ordered root.
`BackgroundExecutor` selects and orchestrates each finite workload, while the
generated root owns the compiler-fixed node/materialization order inside that
workload. `BackgroundEvaluationPlan` retains
compact runtime binding slots plus each direct/materialization operation's placement
before or after the relevant node. Around an applicable node invocation, generated
code calls narrow transaction-supplied prepare/finalize hooks carried by
`BackgroundEvaluationCall`. The hook receives only an opaque transaction-local
operation frame; it may realize already-planned views and execute conversion,
projection, fan-in or deterministic merge operations, but it receives no executor,
page-store or transaction-owner pointer. Runtime code must not reconstruct this
schedule by walking `ConfiguredGraph` or lowering internals.

The immutable plan retains the replay binding slot and applicable root/primitive
maximum block size. The transaction lowers its dynamic replay requirements to an
explicit invocation schedule split to that limit. The imported generated
`tick_block()` wrapper is never called once with an arbitrarily coalesced coverage
region that exceeds its compiled block contract.

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

### Manual-control dynamic-input variants

The planned manual-control application of this mechanism is specified in
[node_presentation_and_manual_controls_direction.md](./node_presentation_and_manual_controls_direction.md).
A settled participating manual value is a constant-specialization candidate.
Hover/gesture intent may request a code variant whose exact `dynamic_inputs` set
contains the controls that must be read dynamically; simultaneous gestures form a
set rather than one optional control. Stale LLVM results carry exact request/revision
identity and are discarded when superseded. On release, a settled constant variant
is requested again while the current dynamic variant may continue reading the final
unchanged scalar until replacement is ready.

All such variants are code-only variants of one structural realization: they share
the same canonical `NodeLayout`/`NodeStorage` and must preserve compatible persistent-
state semantics, not merely identical offsets. No recent-variant cache is required
initially; add one only if later profiling justifies it.

## Lowering boundary

The lowering result should carry immutable metadata sufficient for runtime work
without rediscovering project topology. In addition to ordinary Tick schedule
metadata it needs, as applicable:

- stable background-planning port identities and generation-local indices;
- output production and retention per output, plus destination access/delivery facts per connection contribution;
- generated batched forward/reverse/tock traversal entrypoints and constant
  context-layout facts;
- background evaluation component/order information;
- compact runtime binding-slot maps for background and Tick invocation frames;
- explicit before-node/after-node placement of direct-view, conversion,
  projection, fan-in and deterministic event-merge operations;
- replay invocation binding slots and applicable compiled maximum block sizes used
  to build legal transaction-local schedules;
- per-node/per-port background evaluation transaction accumulator offsets/layout;
- semantic SCC IDs/validation products;
- canonical persisted-page store bindings for Tick/persisted and Tock/persisted outputs;
- persisted-output reverse-cut/page-validity binding facts;
- port-atom incidence partitions plus joined source/target storage-capability facts;
- stable concrete-node port-state identities and cold realization descriptors for
  input history and output history/latency, including their valid semantic ranges;
- optional transition-realization requirements and finite expiry positions when a
  steady binding cannot directly represent inherited node-owned state;
- direct/transient plans for tock/ephemeral outputs;
- complete persisted-page bindings for Tick/persisted and Tock/persisted outputs;
- realtime-produced queue output identities/bindings and block metadata layout;
- immutable page-version bindings/publication metadata and reader-pin requirements;
- producer-queue provisioning/publication/reclamation integration facts; and
- event fan-in ordering/capacity facts.

Project sample rate is part of background-computed output semantics and must be supplied to
`tock_coverage()` and propagation where mappings depend on it.

## `CompiledGraph`

`CompiledGraph` remains the immutable JIT artifact: generated machine code plus
compiler metadata. The name describes compilation, not random-access semantics.

It owns no mutable dynamic persisted-output or transaction data. It describes
how a generation binds to executor-owned canonical persisted-page storage,
Tick-capture resources, and transaction workspaces. It also carries the cold
port-state realization metadata needed to reconcile concrete-node-owned history and
latency across graph revisions; that metadata is not part of the hot generated
storage-access path.
Stable stored-output identity is derived from stable project node/member/output
identity, not generation-local primitive IDs. This applies to tick/persisted as
well as tock/persisted outputs; neither is a best-effort cache.

## Executor boundaries

GraphJit produces immutable compilation artifacts; mutable runtime ownership lives in
one `GraphExecutor` app module with two internal execution actors.

One successful compilation is prepared as one complete `ExecutionGeneration` before
it can become pending. The prepared generation contains the realtime/background
realizations plus all topology-derived routes, producer endpoints, pre-sized
background input descriptors, migration metadata, reserve requirements and cutover
linkage needed by both actors. There is no semantically visible half-staged generation.

### `RealtimeExecutor`

`RealtimeExecutor` owns:

- the active and optional pending `ExecutionGeneration*` realtime realization and
  canonical realtime `NodeStorage`;
- graph-revision realtime state reconciliation, transition/steady realizations and
  safe-boundary activation;
- callback/pass-lifetime immutable persisted/materialization views;
- producer-specific `ProducerReserve` endpoints for realtime-to-background work; and
- at most the newest compatible pending immutable persisted-state pointer from
  background.

Executable replacement must preserve concrete-node-owned port state. Input history
belongs to the surviving destination input; output history and authored latency/future
state belong to the surviving source output. Connection rewiring, fan-in/fanout
changes, or a different steady storage representation do not themselves reset those
windows. `RealtimeExecutor` reconciles overlapping valid semantic ranges from the old
realization into the new revision, materializing transition-only state when necessary.

Realtime-produced data uses blocks already provisioned into the producer reserve. The
producer may write final audio/event payload directly into those blocks, build a
private chain with ordinary stores, and publish the complete chain at a pass boundary.
The actor never allocates/resizes/reclaims on the realtime path.

### `BackgroundExecutor`

`BackgroundExecutor` owns:

- its worker thread and current background generation;
- ordered `ExecutionGeneration*` cutovers already performed by realtime;
- background-only mutable evaluation/Tock state;
- canonical persisted/recorded state and immutable publication roots;
- one producer-specific background `PendingQueue` per compiled producer;
- pre-sized descriptors that remember finite `(first,last)` selected queue prefixes;
- propagation/evaluation workspaces and transaction-local candidates; and
- persisted-state version production.

Each pending queue is pinned independently. There is deliberately no atomic snapshot
across queues. Once the selected terminal block and immutable non-queue inputs have
been remembered, current background work is immutable while producers continue
publishing later chains.

The background-to-realtime result path is a latest-version mailbox, not a queue of
versions. When several coherent immutable persisted versions are produced before
realtime reaches a pass boundary, only the newest compatible pending version must be
retained. Superseded versions are reclaimed off realtime.

### Generation cutover

At a legal realtime pass boundary, `RealtimeExecutor` publishes all final old-generation
chains, publishes the already-prepared successor `ExecutionGeneration*` as an ordered
cutover, then swaps its active realtime realization. No dynamic allocation is required
at that boundary.

Generation objects themselves may carry intrusive successor linkage, avoiding a
separate cutover allocation. Once an actual cutover occurs it must remain ordered while
background lags. Old-generation endpoints are then closed, giving their pending queues
finite tails. Background finishes/drains old-generation work under the old graph,
applies the prepared stable-identity migration, then consumes new-generation queues.

A removed producer therefore simply stops producing after the boundary; it does not
erase retained state at a surviving destination. A removed destination is retired only
after remaining old-generation work has been interpreted under the old graph. Stable
logical identity, not installation order or generation-local indices, determines the
result.

### Capacity/reclamation infrastructure

`AsyncCapacityManager` owns off-realtime block provisioning/reclamation. Producer
requirements are expressed by the maximum burst `C`, low ready-capacity watermark `L`
and refill target `H`; allocator slab granularity is manager-internal. `C` validates or
derives policy and normal operation maintains ready reserve above `L`, refilling toward
`H`.

The forward path remains one reserve/pending queue per producer. Because there is one
background releasing actor and one capacity manager, released blocks from all producer
queues may share one SPSC return stream. Block metadata identifies which reserve/type
receives recycled capacity.

The detailed normative contract is in
[realtime_background_execution_and_queues.md](./realtime_background_execution_and_queues.md).

## Failure semantics

A project mutation may successfully update desired/configured project state and
still fail during `GraphJit` compilation.

In that case:

- `ProjectGraph` retains the desired revision and compile diagnostics;
- neither `RealtimeExecutor` nor `BackgroundExecutor` is given a partial failed generation;
- the previous complete executable generation may continue running;
- a later project/definition change retries the entire root-build transaction.

Desired graph revision and active executable revision are therefore distinct
state even though compilation itself is synchronous.

Runtime staging after a successful compile is also transactional with respect to
activation. `ProjectGraph` stages background first. If that fails, realtime is not
staged. If background staging succeeds but realtime staging fails, no cutover occurs;
the staged background target has never become semantically active and may be reclaimed
or superseded off realtime. The previous active generation continues in either case.

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

Realtime node state and compiler-selected audio-thread regions whose contents cross
audio-root calls become one packed `NodeLayout`/`NodeStorage`. Invocation-local
realtime representations instead use the statically packed generated-root stack
frame. Background-owned state (`TockState`, evaluation workspaces, persisted pages,
etc.) is intentionally outside this realtime allocation model and follows its own
lifetimes. Pure realtime storage analyses may still decide which logical values need
stack or `NodeStorage`, their size/alignment, liveness, and desirable layout before
LLVM/declaration generation; they do not create a parallel realtime state arena.

See [sequential_port_storage_planning.md](./sequential_port_storage_planning.md) for
the rule that logical connections do not imply buffers, and
[coverage_and_background_evaluation.md](./coverage_and_background_evaluation.md) for exact coverage/change
propagation, whole-page covered-domain validity/versioning, reverse demand, and
batched tock semantics that lowering specializes.
