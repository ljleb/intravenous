# Realtime Port Storage And Connection Planning

_Status: current design direction for whole-project realtime sample/event lowering._

Related documents:

- [graph_jit_direction.md](./graph_jit_direction.md)
- [builder_lowering_pipeline_design.md](./builder_lowering_pipeline_design.md)
- [indexed_dsp_nodes.md](./indexed_dsp_nodes.md)
- [intravenous-llvm-hot-reload-and-whole-graph-design.md](./intravenous-llvm-hot-reload-and-whole-graph-design.md)

## Core rule: a connection is not a buffer

`ConfiguredGraph` describes the graph that should exist. A sample/event
connection is logical dataflow semantics, not a declaration that a physical
buffer must be allocated.

At the physical buffer level there are only two storage locations: the generated
root's fixed stack frame and persistent `NodeStorage`. They produce three useful
plans: an entirely stack-resident buffer, a stack working buffer plus exactly the
history/latency carry that must persist in `NodeStorage`, or an entire persistent
buffer in `NodeStorage`. This applies to an event stream and, independently, to
each sample channel group.

Aliasing, direct forwarding, fan-in, conversion, feedback delay, and SCC work
placement are operations over those buffers, not additional storage plans. LLVM
may subsequently eliminate a buffer or scalarize it, but physical planning does
not depend on that optimization.

The compatibility runtime may continue to use ring-buffer-backed
`InputPort`/`OutputPort`/shared-port objects. That implementation must not become
the semantic contract of `ConfiguredGraph` or the generated whole-project
kernel.

## Node-facing realtime APIs express logical access

A node author should use the same `tick_block()` port API regardless of the
chosen physical representation.

Conceptually:

```cpp
auto input = ctx.input<"input">();
auto output = ctx.output<"output">();

output[i] = process(input[i]);
```

must not mean "index a ring buffer." It means "access the logical realtime port
at this sample position inside the statically legal window."

The retained package LLVM should expose/invoke simple recognizable port access
operations that `GraphJit` can specialize after the whole graph is known.
Avoid forcing the node ABI through runtime virtual calls, erased function
pointers, or a required persistent buffer object when the compiler can resolve
the access directly.

After inlining, LLVM must be free to eliminate even a planner-selected transient
materialization when SSA/SROA/loop optimization proves it unnecessary.

The planner therefore chooses the **minimum correct storage requirement**, not a
mandatory final machine representation.

## Persistent generation state uses one `NodeStorage` allocation model

Persistent storage selected by whole-project lowering must feed the existing
`NodeLayout`/`NodeStorage` machinery rather than create a second persistent
graph-kernel arena. The generated project behaves as a zero-input, zero-output
root node whose `declare()` operation declares constituent nodes plus
root/compiler-owned persistent regions into one `NodeLayoutBuilder`.
`GraphExecutor` owns the resulting single `NodeStorage`; invocation-local
temporaries belong to the generated root's fixed stack frame.

Any project-owned data that must survive from one execution call to another
belongs in that layout. This includes history/latency carry, full fixed persistent port buffers,
feedback state, `State`, optional tock-only non-semantic `IndexedState`, and
activity state.
Invocation-local port temporaries do not acquire persistent ownership merely
because their maximum size is known: the generated root should reserve them in
its fixed stack frame, subject to a compile-time stack budget, or choose a full
`NodeStorage` representation for that port group. The audio thread never grows
either storage class dynamically.

The builder should expose a low-level aligned raw-region declaration operation
for generated root code. Unlike authored `local_array()`, such a region need not
correspond to a typed `std::span` field; generated LLVM may address it by the
constant offset fixed by the completed layout. It is still an ordinary
`NodeLayout` region and participates in the same one-allocation ownership model.

Physical region order is a compiler choice and may be selected for locality of
the optimized tick/access programs. Lifecycle order remains a separate
`NodeLayout` concern derived from declaration dependencies. Compiler-owned raw
regions that require a defined fresh value must declare a raw-region initializer
and receive that value through `NodeStorage::initialize()` before activation; the
generated realtime root must not substitute a first-call/run-once guard.

Truly request-sized caller data whose maximum size is not known at graph compile
time is not a legal realtime-port backing strategy. It belongs to a non-realtime
request boundary and does not justify a second persistent project storage
abstraction.

## History and latency are semantic windows, not storage classes

Node source should not need different `tick_block()` code merely because a port
uses history or latency.

History/latency analysis determines what values must remain observable and for
how long. The storage planner then chooses an implementation.

For example, if a stream needs four past samples and a 256-frame current block,
a legal implementation may be:

```text
4 persistent carry samples
+
256 transient current-block samples
```

rather than one large persistent ring.

For larger windows or cost profiles, a ring may be cheaper. Feedback may impose
its own persistent state. Some latency can be represented by scheduling/state
choices rather than by materializing every logical edge.

The compiler should make that decision from graph facts and a cost model rather
than exposing a storage choice in the node declaration.

## Plan producer connection groups, not isolated edges

Storage choices are shared across fanout, so the planning unit should normally
be one producer/output and all of its consumers rather than each edge in
isolation.

Example:

```text
producer
   +--> identity consumer A
   +--> identity consumer B
   +--> converted consumer C
```

may use one transient producer representation aliased by A/B and one conversion
materialization for C. If one consumer requires persistent history, the planner
may choose a persistent source representation while keeping conversion scratch
transient.

Planning one edge at a time would miss these sharing opportunities and can
force unnecessary copies.

## Separate correctness requirements from heuristic choice

Use two deterministic pure stages.

First derive the legal requirements:

```cpp
ConnectionStorageRequirements
 derive_connection_storage_requirements(ConnectionGroupFacts const&);
```

This stage answers correctness questions such as:

- must any value survive across `tick_block()` invocations?
- must it survive across audio passes?
- what history and corrected latency ranges can consumers read?
- does feedback/SCC execution impose persistent state?
- are producer/consumer representations directly aliasable?
- does a conversion require materialization or can it remain arithmetic?
- does a concrete system/communication node require ordinary retained state for
  its external resource interaction?
- what is the pass-local live interval?
- what temporal window is legal for realtime event production/consumption?

Then choose among the legal storage plans using two explicit pure policy
functions, one per payload class. Their result contains the common storage kind
plus payload-specific capacity and layout facts:

```cpp
SampleConnectionStoragePlan
choose_sample_connection_storage_plan(
    SampleConnectionStorageRequirements const&);

EventConnectionStoragePlan
choose_event_connection_storage_plan(
    EventConnectionStorageRequirements const&);
```

The storage chooser should enumerate legal fixed-capacity candidates and compare
their copy work for the complete producer group. Storage lifetime/residence is
one axis; conversion, merge, delay, and aliasing are separate operation axes.
The former `direct`/`transient_sequence`/`compact_persistent_carry`/
`persistent_ring`/`feedback_ring` enums conflated those axes and have been
removed rather than retained as compatibility aliases.

The configured project root itself has no boundary ports. Device I/O and
communication with other application modules are modeled by concrete node types,
so root-boundary handling is not a port-storage implementation kind.

### Realtime port storage plans

There are three useful physical storage plans for an event stream or a sample
channel group:

| Storage plan | Invocation-local storage | `NodeStorage` | Copies caused by retention |
| --- | --- | --- | --- |
| transient | One fixed-capacity buffer in the generated root stack frame. | None for the stream. | None. |
| transient with persistent carry | One fixed-capacity working buffer in the generated root stack frame. | Exactly the history/latency tail that must cross root calls. | Restore the retained tail into the working buffer and commit the next retained tail back out. |
| full persistent | None is required merely to reconstruct the stream. | One fixed-capacity buffer covering the complete simultaneously-live window. | No root-boundary reconstruction copies; producer and compatible consumers address the persistent buffer directly. |

A shared physical-planning vocabulary can therefore begin with:

```cpp
enum class RealtimeBufferStorageKind {
    transient_stack,
    stack_with_persistent_carry,
    full_node_storage,
};
```

Sample and event plans then add their payload-specific capacity/layout facts and
explicit operations. `direct`, `converted`, `merged`, and `feedback` describe
how representations are related or scheduled; they are not values of this enum.

The stack-versus-full-persistent choice may also respect a compile-time stack
budget. It must never fall back to a runtime allocation. Every selected buffer
has a capacity fixed while compiling the graph.

`EventInputPort` and `EventOutputPort` require one power-of-two event span plus
read/write indices. They do not require that span to live in `NodeStorage` and do
not require a segmented carry/current view. For the carry plan, GraphJIT restores
the carry into the one stack working buffer before the relevant callback or
conversion; the port still sees one buffer. For the full-persistent plan, it sees
the one `NodeStorage` buffer directly. Generated bindings should contain or
materialize the already-selected concrete pointer, so fetching a port does not
branch on its storage class.

The following are operations over those representations, not additional storage
categories:

- identity/direct fanout aliases an existing representation and performs no copy;
- fan-in performs an explicitly planned stable merge and chooses a producer home
  that minimizes moved events;
- sample conversion aliases existing channels when conversion is only layout,
  permutation, or duplication; arithmetic conversion materializes only the
  computed result channels, with equivalent fanout results shared where planned;
- block/SCC adaptation determines when that materialization runs;
- feedback is a delayed derived stream whose retained storage uses the same carry
  versus full-persistent alternatives; and
- external I/O belongs to concrete nodes, not a root connection representation.

Every planned event operation also owns an explicit execution scope. A
primitive `before`/`after` scope uses that primitive invocation's index and block
size and therefore repeats for SCC slices. A region `before`/`after` scope uses
the complete root-call window and executes once outside the slice loop. Scope is
part of representation sharing: two otherwise identical conversions do not
share a derived buffer when one is required after every producer slice and the
other is required once at SCC exit. Execution planning consumes these scopes
directly; it must not inspect downstream bindings and infer placement after
physical storage has already been chosen.

This gives the event pipeline a fixed order of decisions:

1. derive semantic source/target windows and SCC relationships;
2. enumerate direct, conversion, merge, and delay operations with their scopes;
3. establish representation readers and writers;
4. derive exact capacities and cross-invocation lifetimes;
5. compare the legal stack/carry/full-storage alternatives and copy work; and
6. pack transient lifetimes and emit resolved bindings.

For a cyclic producer with source history or latency, the callback writes an
invocation-local bounded sequence. A slice-scoped stable merge inserts it into
the canonical retained aggregate. This is necessary because a later slice may
legally author an event whose timestamp precedes a future event authored by an
earlier slice; direct append would violate the sorted-stream invariant. The same
mechanism extends to same-SCC fan-in. It uses only statically sized buffers. The
local capacity is derived from `SCC quantum + history + latency`; the aggregate
capacity accounts for the root block plus the history/latency allowance for
every possible slice, since each callback invocation may legally fill its whole
declared authored window.

For samples, the buffer unit is a channel. Identity channel routing, projection,
permutation, layout-only conversion, and channel duplication bind existing channel
storage directly. Arithmetic conversion reads its semantic input channels from
their resolved representations without a contiguous input gather and materializes
only result channels that cannot be expressed as aliases. The same three storage
plans then apply per canonical or derived channel group.

Candidate selection should use the actual worst-case copied event/sample counts
for the complete fan-in/fanout group. A fixed event-count threshold such as 64 is
not the policy contract. For the simple single-source case, carry is useful when
the retained history/latency tail is small relative to the current-block working
set; full persistent storage becomes preferable when restoring and committing
that tail costs more than addressing the entire retained buffer in place. A
sensible initial sample crossover considers carry only while retained frames are
less than one full block, then lets the whole-group copy model choose full
persistent earlier when appropriate. The event equivalent compares the actual
rate-derived retained-event capacity with the actual rate-derived current-block
capacity; it never substitutes a fixed event count for either quantity.

This crossover policy is intentionally isolated so benchmarking can change it
without changing graph semantics or LLVM lowering.

### Current implementation status

The storage-model and physical-residence refactors have landed:

- sample and event producer groups now select the shared three-kind storage
  model, while event invocation aggregation is a separate operation fact;
- ordinary event capacities start from
  `ceil(max_events_per_index * temporal_span)` and are rounded to a power of
  two;
- the fixed 64-event and 16-KiB thresholds are gone. The shared pure chooser
  now enumerates transient, carry, and full-persistent candidates, exposes their
  copied bytes, ring-addressed bytes, stack footprint, persistent footprint, and
  weighted cost, and accepts candidate-specific whole-group copy counts plus a
  configured stack-budget constraint. `GraphJit` now propagates one configured
  cost model through connection, sample, event, and feedback planning. After the
  sample/event stack buffers are independently lifetime-packed, lowering lays
  those two byte ranges into one generated-root stack allocation and enforces
  the budget against that final byte count. If it does not fit, lowering raises
  the stack-byte cost and re-runs storage selection so eligible producer,
  history/latency, feedback, and disconnected-output buffers move to
  `NodeStorage`; a final minimum-stack pass either fits or reports that the
  remaining mandatory conversion/merge buffers exceed the budget. The default
  weights preserve the landed block-relative/rate-derived crossover until
  topology-specific costs justify an earlier full-persistent choice;
- invalid event rate/span capacities fail connection planning immediately;
- ordinary full persistent rings are fixed at compile time from the producer
  rate and `history + block + latency`; no current event strategy grows a ring
  dynamically on the audio thread;
- sample and event transient representations are packed independently by their
  inclusive schedule live intervals into fixed-size generated-root stack
  arenas. Dead ranges may reuse the same bytes; neither arena is a
  `NodeLayout` region or migration state;
- compact sample/event carry and full persistent buffers remain canonical
  `NodeStorage` raw regions. Event producer overflow counters are separate
  persistent regions, so telemetry survives even when the producer sequence is
  transient;
- reflected sample/event bindings now contain already-resolved representation
  pointers. The generated root resolves stack versus `NodeStorage` residence
  while emitting straight-line LLVM; node wrappers do not branch on a storage
  kind or reconstruct an address from one universal storage base;
- detached sample and event feedback now derive the exact retained delayed
  span and use the same three-kind planners as ordinary history/latency. Compact
  feedback carry stores only the cross-invocation tail in `NodeStorage`; full
  feedback storage remains a fixed persistent ring, and zero-capacity event
  feedback can remain transient. Event capacities are rate-times-live-span rather
  than `source_capacity * (loop_extra_latency + 1)`;
- event lowering now performs a physical-operation costing pass before final
  residence realization. Shared conversion/materialization writes are counted
  once per emitted operation, full-`NodeStorage` candidates include the ring
  reads those operations perform, and identity fanout adds no copy. Each shared
  delayed feedback stream accounts once for producer-to-feedback writes, exact
  compact-tail restore/commit work, persistent-ring addressing, and any shared
  consumer conversion. Fan-in producer-home and separate-target alternatives
  receive the same downstream operation costs before the final choice;
- event fan-in whose producers span execution regions is staged into one
  canonical aggregate. Because execution order need not match semantic source
  order, only these canonical staged aggregates carry a fixed-capacity parallel
  source-ordinal array. Merge, compact-carry, and persistent-ring operations
  preserve that compiler-private metadata; `InputPort` and `OutputPort` still
  expose only the ordinary `TimedEvent` payload buffer and require no storage
  dispatch.

The remaining cost-model work is primarily alias-versus-materialize comparison,
weight calibration, and making stack-pressure promotion choose more selectively
when several different storage moves can satisfy the same budget.

The heuristic may consider:

- block size;
- value/event size and alignment;
- channel layout;
- fanout;
- history/latency depth;
- expected copy volume;
- stack budget;
- scratch-memory pressure;
- cache-line/cache-footprint estimates;
- ring-addressing cost;
- conversion cost;
- target CPU/vector characteristics.

Correctness must not depend on heuristic weights.

These functions belong in ordinary graph/compiler code, not in ORC state, and
should be exhaustively unit-testable without constructing a JIT.

## Transient storage is globally reusable

After implementation selection, pass-local materializations should undergo a
separate liveness/scratch-allocation pass.

If transient A is dead before transient B becomes live, they may share the same
scratch slot. This is analogous to register allocation at block-buffer
granularity.

A useful pure interface is conceptually:

```cpp
ScratchAllocationPlan
 assign_scratch_slots(span<TransientStorageRequirement const>);
```

The word "scratch" describes lifetime, not a second persistent runtime storage
object. A temporary may disappear into SSA/registers or occupy a statically
sized range in the generated root stack frame. If the fixed stack budget makes
that plan unsuitable, the storage chooser may instead select a full-buffer
`NodeStorage` representation; it must not silently put nominally transient
storage into persistent state after physical planning.

The important properties are:

- no realtime heap allocation;
- sizes, alignments, offsets, and lifetimes are known before execution;
- unrelated stack temporaries may reuse one stack-frame range when their live
  intervals do not overlap;
- a full-buffer `NodeStorage` choice is explicit and participates in ordinary
  generation migration only when its contents are semantically retained; and
- the compiler may order stack slots and persistent regions for hot-path
  locality.


## Event conversions are directional semantic conversions

Event conversion planning must fail when producing the target payload would
require inventing information. The built-in conversion graph is therefore
directional rather than a best-effort complete graph.

The intended built-in relations are:

```text
MIDI     -> Boundary, Trigger, Empty
Boundary -> Trigger, Empty
Trigger  -> Empty
Empty    -> (nothing except identity)
```

`Trigger -> Boundary` is invalid because a trigger carries no duration/end
semantics. `Trigger -> MIDI` and `Boundary -> MIDI` are invalid because a note,
channel, velocity, and related MIDI details cannot be chosen objectively.
Conversely, information-rich event types may collapse into `Trigger`, and any
event type may be discarded into `Empty`.

This rule also removes the old conversions which synthesized a second event at
`t + 1`; current built-in conversions never invent a later timestamp. Realtime
window validation nevertheless checks converted events at the point they are
emitted, so future conversion additions cannot silently escape the legal
window.

## Realtime event ports need bounded time windows

Realtime event outputs must have a statically predictable temporal window just
like realtime sample outputs.

A realtime output callback must not be able to produce an event at an arbitrary
absolute time unrelated to the current invocation. Its legal output timestamps
must lie inside the finite window defined by the current block together with the
port's declared history and declared/corrected latency. In other words, realtime
event production is constrained by the same `current block + history + latency`
semantic extent used to make realtime sample access predictable.

For a callback beginning at global sample index `B`, block size `N`, output
history `H`, and effective/corrected output latency `L`, the legal realtime event
output extent is the half-open interval:

```text
[max(0, B - H), B + N + L)
```

The compatibility runtime enforces this exact half-open convention. Lowering
may remove redundant checks when authored/generated accesses are statically
proved to stay inside the same extent.

The compatibility runtime should validate this constraint. The whole-project
JIT may then specialize it away when the authored access is statically valid.

Arbitrary `TimedEvent` insertion outside that window is not part of the future
realtime port contract.

## Realtime timing is an orthogonal access config

History and latency are not sample-payload properties and should not be
duplicated into event-payload properties. They describe the finite temporal
contract of **realtime access**, regardless of whether the payload is samples or
events.

The port declaration therefore has two orthogonal axes:

```cpp
struct RealtimeInputConfig {
    std::size_t history = 0;
};

struct RealtimeOutputConfig {
    std::size_t history = 0;
    std::size_t latency = 0;
};

struct IndexedInputConfig {};
struct IndexedOutputConfig {};

enum class OutputRetention {
    ephemeral,
    persisted,
};

using InputAccessConfig =
    std::variant<RealtimeInputConfig, IndexedInputConfig>;
using OutputAccessConfig =
    std::variant<RealtimeOutputConfig, IndexedOutputConfig>;

// Conceptually part of the parent OutputConfig, alongside payload properties.
struct OutputConfig {
    OutputAccessConfig access{RealtimeOutputConfig{}};
    OutputRetention retention = OutputRetention::ephemeral;
};
```

Output access and retention are independent:

| output access | retention | production semantics |
| --- | --- | --- |
| `RealtimeOutputConfig` | `ephemeral` | produced by `tick_block()`; no semantic retention requirement |
| `RealtimeOutputConfig` | `persisted` | produced by `tick_block()`; finalized values are retained |
| `IndexedOutputConfig` | `ephemeral` | produced by `tock_coverage()`; no semantic retention requirement |
| `IndexedOutputConfig` | `persisted` | produced by `tock_coverage()`; complete exact coverage is retained |

`RealtimeOutputConfig` always keeps its ordinary history/latency authoring
semantics. Persistence begins only after a position is final according to that
contract; it does not turn the first write into an immutable value and does not
impose a separate whole-block recorder transaction.

`ephemeral` means only that retaining produced values is not semantically
required. GraphJit/GraphExecutor may still allocate bounded transient pages,
prefetch buffers, or other caches when a connection requires materialization.
`persisted` is a runtime retention guarantee and does not by itself imply project-
file serialization or external-file writeback.

`InputConfig` / `OutputConfig` separately carry the sample/event payload variant
and the access variant. Output retention lives once on the parent output config,
not inside either access alternative. `SampleInputProperties`,
`SampleOutputProperties`, `EventInputProperties`, and `EventOutputProperties` do
not carry history or latency. The same distinction is preserved in
`ConfiguredGraph`.

Only realtime access carries finite history/latency. Indexed declarations cannot
accidentally acquire those fields.

For scheduling/storage purposes, a node is stateful across invocations if it has a
nested `NodeState` **or** any realtime input/output history or output latency.
History/latency creates a statically bounded temporal dependency; `NodeState` may
carry a dependency forward without a static bound.

`EventOutputProperties` additionally carries a static event-buffer sizing rate:

```cpp
struct EventOutputProperties {
    EventTypeId type {};
    double max_events_per_index = 1.0;
};
```

`max_events_per_index` is the producer's declared maximum used to derive every
event buffer capacity. For a representation covering `W` simultaneously-live
sample positions, the planner starts from

```text
ceil(max_events_per_index * W)
```

event slots. Fractional values therefore let sparse producers request smaller
static buffers: for example, `0.24` over a 64-sample representation requests 16
event slots. The declaration constrains total capacity for the represented
window, not the distribution of timestamps inside it: all 16 events may occur at
one legal sample position. The value must be finite and nonnegative. `0.0`
declares that the producer emits no events.

Exceeding the declared maximum is outside the realtime producer contract and has
implementation-defined behavior. A particular implementation may drop excess
events and count them, but callers must not depend on that policy. It must never
grow a buffer or allocate memory on the audio thread.

This sizing rate belongs to the event **output payload properties**, not to
`RealtimeOutputConfig`: history/latency define *when* an output may author data,
while `max_events_per_index` lets GraphJIT determine how much static event
storage to reserve for the selected temporal representation.

## Indexed ports use explicit sparse coverage

Do not apply the realtime bounded-window rule to `IndexedOutputConfig`.

Indexed sample/event outputs publish finite `IndexedCoverage`: a canonical union
of disjoint half-open global-index regions. Coverage is the indexed semantic
domain boundary; there is no separate bounding extent. Node callbacks never
request indexed values outside input coverage, so long uncovered timeline gaps
require no storage or computation.

Exact semantic changed/demand regions remain independent of physical storage
pages. For an indexed/persisted output, canonically aligned pages may be wholly
valid or invalid for exactly `page_interval & coverage` while a **candidate**
version is being rebuilt. Forward changed regions are not widened to page
boundaries. A published indexed/persisted output has every covered page domain
valid; sparse requests merely read from that complete representation.

An indexed/ephemeral output owns no persistent output pages. Non-realtime access
may use caller/transaction storage, while realtime lowering may use direct
consumer placement or bounded compiler-owned transient storage when safe.

Realtime and indexed access remain distinct connection domains. A
`RealtimeOutputConfig` does not directly satisfy an `IndexedInputConfig`, and
GraphJit does not insert a generic realtime-to-indexed page/ring adapter. Crossing
that boundary is an explicit node-level operation so the node can define the
capture, overwrite, seek, and retention semantics deliberately.

A recording/capture bridge is the important case. Its realtime side consumes an
ordinary realtime input, while its indexed side exposes an ordinary indexed output.
Whenever a recording output block is produced during `tick_block()`, the generated
realtime path immediately copies that block into already-provisioned capture
storage. Capture does not wait for the end of the root tick. Each captured record
carries at least:

```text
CaptureSequence
OutputPortId
GlobalBlockPosition
payload block
```

`CaptureSequence` is monotonically increasing insertion order in the executor's
shared recording-capture log. `OutputPortId` identifies the indexed bridge output
whose value/coverage is affected, and `GlobalBlockPosition` identifies where that
change belongs. Global positions need not increase with sequence: seeking during
playback may append a new capture for an earlier position, and consecutive captures
may belong to different output ports.

Capture exists only while playback/recording is active; the final duration of one
run need not be known in advance. Storage is slab-backed and dynamically extensible
without requiring a reallocation of earlier slabs. The audio thread is only a consumer of
pre-provisioned free blocks: it acquires one, copies the produced block, attaches
metadata, and publishes the capture record. A separate non-realtime allocation
worker maintains a target amount of free realtime-consumable capacity by allocating
reasonably sized slabs independently of indexed execution. Slow propagation/tock
therefore increases the captured-but-unprocessed backlog rather than consuming a
fixed compiler-planned bridge window. The allocator may recycle blocks returned by
completed indexed transactions.

The indexed worker snapshots a fixed contiguous **capture-sequence prefix** at the
start of each propagation/tock pass. Contiguous here refers only to insertion
sequence; the selected records may cover arbitrary ports and nonmonotonic global
positions. Captures published after the snapshot cutoff are excluded from the
running pass and belong to a later pass.

The selected records are coalesced into exact changed coverage keyed by output port
and seed the normal indexed forward-propagation machinery. Reverse planning and
`tock_coverage()` then run normally for all affected nodes and page domains. The
complete propagation/tock transaction builds candidate indexed pages and atomically
publishes one new pages version. Capture insertion itself is **not** indexed
publication and does not advance the pages version.

Published indexed versions own/materialize the data they need and never retain
references into capture storage. After a successful transaction commits its fixed
capture prefix, those consumed capture blocks may therefore be returned to the
allocator immediately. If work is cancelled or rejected as stale, the processed
capture frontier does not advance and the corresponding blocks remain available
for a later transaction.

Changing the root block size is a quiescent physical-layout transition, not an
indexed semantic invalidation. Persistent indexed values are losslessly
repartitioned as needed, a replacement GraphJit generation receives the new
canonical layout, and publication switches only after migration completes.
Semantic versioning and physical layout generation remain distinct.

Persistence does not alter `tick_block()`'s legal history/latency writes: only
finalized positions acquire the retention obligation. Nor does persistence imply
indexed accessibility. Realtime/persisted storage remains an output-retention
concern; an indexed consumer requires an explicit bridge node whose indexed output
participates in the ordinary indexed transaction model above.

Persistent stored sample payloads may be dense or coverage-packed. Stored event
payloads are packed ordered events; event fan-in order is deterministic by
absolute sample index, stable source/connection ordinal, then producer-local
order. Combined live event-buffer capacities must account for all incoming
`max_events_per_index` bounds.

See [indexed_dsp_nodes.md](./indexed_dsp_nodes.md) for the normative indexed
execution/publication semantics.

## Event storage planning mirrors sample storage planning where possible

Once realtime event windows are bounded, event connection storage can also be
selected from graph facts rather than fixed globally.

Examples:

```text
feed-forward zero-history event stream
    -> transient immutable event sequence or fused/direct handling

identity fanout
    -> several consumers share one immutable produced sequence

converted consumer
    -> conversion scratch only for that branch

latency/history/feedback event stream
    -> persistent event retention sufficient for the required window
```

A representation's temporal span and the producer sizing rate determine its
static event capacity. For a representation covering `W` sample positions from
a producer with `D = max_events_per_index`, GraphJIT starts from
`ceil(D * W)` event slots. The current bounded-sequence representation rounds
that count upward to a power of two because `EventSharedPortData` uses a ring
mask. Every realtime event representation must have such a finite compile-time
capacity. Failure to represent the calculated capacity is a graph-compilation
error, not a reason to select a dynamically sized fallback.

The storage chooser's **current** event count and an SCC feedback buffer's
**authored** event count are deliberately different bounds. The current count is
the maximum for one generated-root block and is compared with the state retained
between root calls when choosing transient, carry, or full storage. A sliced SCC
may author across a larger combined history/latency horizon during that root
call; that authored bound sizes the physical feedback working buffer and its
append work. Using the authored bound as the chooser's current footprint would
make large source latency incorrectly render compact carry eligible even when
the retained state is larger than one root block.

The span depends on the selected storage plan:

- a transient producer buffer covers the maximum events that can be newly
  authored into that invocation's legal output window;
- persistent carry covers exactly the history/latency interval crossing root
  calls;
- the carry plan's stack working buffer covers the restored carry plus the
  maximum newly authored events that may coexist with it;
- a full persistent buffer covers the complete simultaneously-live interval,
  including current block, history, authored latency, and any feedback delay
  owned by that representation; and
- an independently delayed feedback branch is sized from its delayed
  simultaneously-live interval and source rate, not by multiplying a source
  invocation buffer by a guessed number of outstanding callbacks.

Fan-in sums the separately calculated source maxima before physical rounding;
fanout does not multiply capacity. A non-expanding converted branch never needs
more event slots than the source events visible to that operation, although it
may conservatively inherit the source representation's capacity. These formulas
do not constrain timestamp distribution within the representation: every event
covered by the declared maximum may legally share one timestamp.

The older `calculate_event_port_buffer_capacity(...)` value-size heuristic is
therefore no longer the GraphJIT capacity calculation. Heuristics remain useful
only for choosing among already-sized storage candidates; they never replace the
rate-times-live-span formula.

Fanout does not multiply the sizing rate: several consumers of one logical
producer share the same source event stream. A merge of independent producers
sums their rates for the merged representation. Realtime event producers are
contractually sorted by nondecreasing absolute sample index. For transient
feed-forward multi-producer fan-in, semantic source 0 writes directly into the
canonical aggregate allocation while retaining its own logical producer
capacity; the other sources use bounded local sequences. After the last producer
completes, one backwards k-way merge writes the final sorted aggregate in place.
Each final event is therefore written at most once by merge rather than being
rewritten through an incrementally growing pairwise aggregate. Equal-timestamp
events retain deterministic semantic-source ordering, and conversion/fanout
operate on the merged stream. Retained fan-in intentionally keeps the existing
separate-target realization for now; cyclic multi-producer fan-in remains a
separate SCC capability. Implicit event
conversions are required to be
**non-expanding**: each source event produces zero or one target event, timestamps
are preserved, and the conversion may only preserve or discard information. Any
transformation that can synthesize multiple events belongs in an explicit node,
whose own output declares its resulting sizing rate.

Retained canonical event storage and transient conversion are composable rather
than mutually exclusive. A compact-carry working sequence or persistent ring may
feed a transient branch that selects the current root interval plus that branch's
declared input history before applying its conversion. This avoids repeatedly
converting retained events that the consumer cannot observe during the current
root invocation, while the canonical retained representation continues to serve
identity/history consumers directly.

The selected temporal interval does **not** by itself justify assuming that
events are evenly distributed across samples. Every event covered by the
producer's declared maximum may legally share one timestamp inside the selected
interval. For a non-expanding implicit conversion, source-sized derived capacity
is therefore a conservative allocation that cannot overflow merely because
events are temporally clustered. Identical retained conversion branches may
still share that one derived representation.

### Event work placement and avoidable-work rules

Physical lowering schedules event work at the narrowest lifetime that is still
semantically correct:

- an ordinary producer materialization runs once after that producer;
- a cyclic producer appends across slices into one root-call aggregate rather
  than resetting and rebudgeting a sequence for every slice;
- a derived sequence consumed inside that SCC is refreshed after each producer
  slice using the current slice index and size;
- a derived sequence consumed outside that SCC is materialized once at region
  exit from the complete root-call aggregate;
- persistent-ring pruning and compact-carry restore run once at root/SCC entry;
- compact-carry commit runs once at root/SCC exit; and
- detached feedback appends only the newly authored suffix after its producer,
  never the restored retained prefix.

These placements are part of the efficiency contract. Moving them to a more
frequent scope can preserve simple test cases while repeating conversion/copy
work, inflating static event budgets, or duplicating events in feedback state.

Current event lowering packs invocation-local event buffers into one fixed
root-stack allocation. Each buffer is live only from its first scheduled access
to its last scheduled access: producer-local fan-in buffers end at the merge,
conversion/materialization results begin when they are written and end at their
last consumer, and feedback working buffers include their restore/append/consume/
commit steps. Buffers whose intervals do not overlap may use the same stack
bytes. An event output that appends across SCC slices remains live across the
whole SCC root invocation; per-slice conversion buffers do not inherit that
longer lifetime. Buffers that preserve events across root invocations and
per-producer overflow telemetry belong in `NodeStorage`.

The current implementation gives each logical event output one saturating
overflow counter and drops an event when that output's sequence is full. That is
one allowed implementation-defined response to a producer exceeding its declared
maximum, not part of the authored-port contract. Derived
conversion/materialization fanout does not duplicate the counter. Realtime
execution must never resize or allocate. Overflow of compiler-owned conversion
or materialization storage while every producer respects its declaration is a
GraphJIT sizing bug, not a producer overflow.

Executor/device telemetry should remain separate when those layers land:
GraphExecutor can count deadline misses, while the audio-device boundary can
count actual input overruns/output underruns. Those conditions have different
causes and should not be collapsed into the event-output overflow metric.

## Suggested planner facts

The pure planner should receive enough facts to make an informed decision rather
than infer them from partially lowered runtime objects.

For samples, useful facts include:

```text
block size
source channel layout
source history/latency
whether the producer reads its own output/history
producer schedule position
consumer schedule positions
consumer input history/corrected latency
conversion plan per consumer
feedback/SCC membership
graph/device boundary status
value size/alignment
```

For events, additionally include:

```text
event type size/alignment
legal produced/consumed time windows
fanout and conversion requirements
producer max-events-per-sample sizing rate
derived event capacity for each temporal representation
consumer retention requirements
```

Target/cost-model facts may include cache-line size, stack budget, preferred
scratch budget, copy/ring/conversion cost estimates, and target vector features.

## Test requirements

The planner should be testable with synthetic facts and no LLVM dependency.
At minimum cover:

- zero-history/zero-latency single-consumer sample flow permits transient/direct
  lowering;
- identity fanout can share one materialization;
- one converted fanout branch does not force all consumers through converted
  storage;
- history requires only the persistent carry actually needed, not an automatic
  full-edge ring;
- large history can select a full persistent buffer under a cost model that
  makes it cheaper;
- feedback/SCC requirements derive an ordinary delayed stream with the necessary
  carry or full persistent state;
- disjoint transient live intervals reuse one stack-frame range;
- tiled/multi-channel layout changes planner facts without changing connection
  semantics;
- realtime event production outside the legal window is rejected;
- every event buffer capacity is derived from producer
  `max_events_per_index` and its exact simultaneously-live temporal span;
- unrepresentable realtime capacities fail planning and never fall back to a
  runtime allocation;
- carry and full-persistent candidates preserve the same event semantics while
  exposing their different copy counts to policy;
- feedback capacity is derived from delayed live span rather than multiplying
  one invocation capacity by a callback count;
- realtime event identity fanout can share an immutable event representation;
- indexed event/sample access remains independent from realtime storage
  planning.

## Compiler pipeline placement

The whole-project compiler order should make the boundary explicit:

```text
ConfiguredGraph logical connections
        |
        v
schedule / dependency / SCC analysis
        |
        v
history / latency / realtime-event-window analysis
        |
        v
derive connection storage requirements
        |
        v
choose sample/event implementation plans
        |
        v
transient liveness + reusable-region allocation
        |
        v
root declaration / canonical NodeLayout planning
        |
        | NodeStorage contains only cross-call state;
        | generated root owns fixed transient arenas
        v
specialized whole-project LLVM
        |
        v
LLVM optimization / ORC
```

`ConfiguredGraph` remains free of physical storage choices. The first place
those choices become concrete is the compiler plan used to generate the whole
project kernel.
