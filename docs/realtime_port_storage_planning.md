# Realtime Port Storage And Connection Planning

_Status: current design direction for whole-project realtime sample/event lowering._

Related documents:

- [graph_jit_direction.md](./graph_jit_direction.md)
- [builder_lowering_pipeline_design.md](./builder_lowering_pipeline_design.md)
- [compiled_dsp_nodes.md](./compiled_dsp_nodes.md)
- [intravenous-llvm-hot-reload-and-whole-graph-design.md](./intravenous-llvm-hot-reload-and-whole-graph-design.md)

## Core rule: a connection is not a buffer

`ConfiguredGraph` describes the graph that should exist. A sample/event
connection is logical dataflow semantics, not a declaration that a physical
buffer must be allocated.

The whole-project compiler may implement a realtime sample connection with:

- direct SSA/register forwarding;
- an aliased producer value/block;
- pass-local stack storage;
- a statically allocated reusable scratch slot;
- compact persistent history/latency carry plus transient current-block storage;
- persistent circular storage;
- explicit feedback/SCC storage;
- external I/O storage;
- an explicitly materialized contiguous block;
- a combination of the above for different consumers.

Likewise, a realtime event connection may be represented by direct/fused event
handling, an immutable transient event sequence shared by several consumers,
conversion scratch, persistent delayed/history event storage, or another
compiler-selected representation.

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

## One generation uses one `NodeStorage` allocation model

Physical storage selected by whole-project lowering must feed the existing
`NodeLayout`/`NodeStorage` machinery rather than create a second graph-kernel
arena. The generated project behaves as a zero-input, zero-output root node whose
`declare()` operation declares constituent nodes plus root/compiler-owned
regions into one `NodeLayoutBuilder`. `GraphExecutor` owns the resulting single
`NodeStorage`.

Any project-owned data that must survive from one execution call to another, or
that is intentionally retained as a bounded reusable workspace, should normally
be represented in that same layout. This includes history/latency carry,
feedback state, persistent event data, `State`, `CompiledState`, activity state,
and compiler-selected reusable temporary regions.

The builder should expose a low-level aligned raw-region declaration operation
for generated root code. Unlike authored `local_array()`, such a region need not
correspond to a typed `std::span` field; generated LLVM may address it by the
constant offset fixed by the completed layout. It is still an ordinary
`NodeLayout` region and participates in the same one-allocation ownership model.

Physical region order is a compiler choice and may be selected for locality of
the optimized tick/access programs. Lifecycle order remains a separate
`NodeLayout` concern derived from declaration dependencies.

Truly request-sized caller data whose maximum size is not known at graph compile
time need not be embedded in `NodeStorage`. This exception does not justify a
second persistent project storage abstraction.

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
- is this graph/device ingress or egress?
- what is the pass-local live interval?
- what temporal window is legal for realtime event production/consumption?

Then choose among the legal representations using two explicit pure policy
functions, one per payload class:

```cpp
SampleConnectionImplementationKind
choose_sample_connection_implementation(
    SampleConnectionImplementationRequirements const&,
    SampleConnectionCostModel const&);

EventConnectionImplementationKind
choose_event_connection_implementation(
    EventConnectionImplementationRequirements const&,
    EventConnectionCostModel const&);
```

The initial policy is deliberately conservative rather than pretending there is
one globally optimal threshold:

- a zero-retention connection uses direct/fused handling when analysis proves it
  legal, otherwise transient materialization;
- feedback uses a persistent ring;
- retained sample payloads use compact carry below a configurable byte budget
  and a ring above it;
- retained event payloads use the `max_events_per_sample` sizing rate to derive
  the retained representation capacity, then use compact carry below a
  configurable count budget and a ring above it; and
- graph/device boundary handling remains a distinct implementation kind.

For events, the two retained implementations intentionally have different copy
behavior. `compact_persistent_carry` keeps a transient producer sequence and copies
only the bounded retained tail into/out of persistent storage at root-call boundaries.
`persistent_ring` is itself the canonical producer representation: producer and
consumers bind directly to one migration-identified power-of-two event ring carrying
monotonic read/write indices. At the start of each root call GraphJIT advances the
oldest retained index past events that are older than the required history boundary;
retained event payloads remain in-place. The ring's static capacity is derived from
the complete simultaneously-live temporal span (`history + current block + latency`)
and the producer's sizing rate.

These crossovers are heuristic policy only. They are intentionally isolated so
benchmarking can change them without changing graph semantics or LLVM lowering.

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

The word "scratch" describes lifetime, not a second runtime storage object. A
small temporary may disappear into SSA/registers or the machine stack. A larger
fixed-capacity reusable slot should normally become a compiler-owned raw region
in the root `NodeLayout` and therefore share the same `NodeStorage` allocation
as persistent node/project state.

The important properties are:

- no realtime heap allocation;
- offsets/lifetimes are known before execution when storage is statically
  reserved;
- unrelated logical connections may reuse one physical region when their live
  intervals do not overlap;
- placing reusable storage in `NodeStorage` does not make its contents
  semantically persistent between calls; and
- the compiler may order regions to improve locality in the generated hot path.


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

struct CompiledPortConfig {};

using InputAccessConfig =
    std::variant<RealtimeInputConfig, CompiledPortConfig>;
using OutputAccessConfig =
    std::variant<RealtimeOutputConfig, CompiledPortConfig>;
```

`InputConfig` / `OutputConfig` separately carry the sample/event payload variant
and this access variant. `SampleInputProperties`, `SampleOutputProperties`,
`EventInputProperties`, and `EventOutputProperties` do not carry history or
latency. The same distinction is preserved in `ConfiguredGraph`; it must not be
flattened back into a `compiled` boolean plus timing fields that are meaningless
for compiled declarations.

This makes invalid combinations unrepresentable: a compiled port cannot
accidentally acquire a finite realtime history or latency.

`EventOutputProperties` additionally carries a static event-buffer sizing rate:

```cpp
struct EventOutputProperties {
    EventTypeId type {};
    double max_events_per_sample = 1.0;
};
```

`max_events_per_sample` is used only to derive a maximum static buffer size for
a known temporal span. For a representation covering `W` sample positions, the
planner starts from

```text
ceil(max_events_per_sample * W)
```

event slots. Fractional values therefore let sparse producers request smaller
static buffers: for example, `0.24` over a 64-sample representation requests 16
event slots. This is **not** a runtime rate limiter and does not impose a
sliding-window constraint on event timestamps; all 16 events may occur at one
sample position if that timestamp is otherwise legal. The value must be finite
and nonnegative. `0.0` requests no event payload capacity for that span, so any
producer attempt necessarily overflows the bounded sequence.

This sizing rate belongs to the event **output payload properties**, not to
`RealtimeOutputConfig`: history/latency define *when* an output may author data,
while `max_events_per_sample` lets GraphJIT determine how much static event
storage to reserve for the selected temporal representation.

## Compiled ports remain random-access

Do not apply the realtime bounded-window rule to compiled access.

Compiled sample ports support arbitrary global sample requests and compiled
event ports support arbitrary global event intervals according to
[compiled_dsp_nodes.md](./compiled_dsp_nodes.md). Their materialization is
request-driven and cannot generally benefit from one static realtime
history/latency retention window.

A compiled declaration therefore carries the empty `CompiledPortConfig`, not a
realtime timing config. The additive rule applies instead to the statically typed
`tick()` / `tick_block()` accessor: a compiled port's current-block wrapper still
exposes the corresponding ordinary sequential operations, while compiled random
access adds the more precise arbitrary-position/range operations.

Compiled access remains an execution capability, not a storage class.

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
a producer with `D = max_events_per_sample`, GraphJIT starts from
`ceil(D * W)` event slots. The current bounded-sequence representation rounds
that count upward to a power of two because `EventSharedPortData` uses a ring
mask. Neither calculation constrains the distribution of event timestamps inside
that representation.

The older `calculate_event_port_buffer_capacity(...)` value-size heuristic is
therefore no longer the primary GraphJIT capacity calculation. Heuristics and
cost-model limits remain useful for choosing among static representations such
as compact carry versus a persistent ring.

Fanout does not multiply the sizing rate: several consumers of one logical
producer share the same source event stream. A merge of independent producers
sums their rates for the merged representation. Implicit event conversions are
required to be **non-expanding**: each source event produces zero or one target
event, timestamps are preserved, and the conversion may only preserve or discard
information. Any transformation that can synthesize multiple events belongs in
an explicit node, whose own output declares its resulting sizing rate.

Retained canonical event storage and transient conversion are composable rather
than mutually exclusive. A compact-carry working sequence or persistent ring may
feed a transient branch that selects the current root interval plus that branch's
declared input history before applying its conversion. This avoids repeatedly
converting retained events that the consumer cannot observe during the current
root invocation, while the canonical retained representation continues to serve
identity/history consumers directly.

The selected temporal interval does **not** justify shrinking the derived event
capacity from the source capacity. Since `max_events_per_sample` is only a static
sizing rate, every event currently resident in the source representation may
legally share one timestamp inside the selected interval. For a non-expanding
implicit conversion, source-sized derived capacity is therefore the conservative
allocation that guarantees materialization cannot overflow merely because events
are temporally clustered. Identical retained conversion branches may still share
that one derived representation.

Each logical event output also owns one saturating overflow counter in its
canonical producer representation; derived conversion/materialization fanout
never duplicates that telemetry. If the statically allocated producer sequence
is full, realtime execution deterministically drops the excess event and
increments that counter; it does not allocate. There is no per-sample or
sliding-window policing beyond the ordinary bounded-buffer capacity. Overflow of
compiler-owned conversion or materialization storage is a GraphJIT sizing
invariant failure and must not be reported as a producer overflow.

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
- large history can select a ring under a cost model that makes it cheaper;
- feedback/SCC requirements force the necessary persistent state;
- disjoint transient live intervals reuse one scratch slot;
- tiled/multi-channel layout changes planner facts without changing connection
  semantics;
- realtime event production outside the legal window is rejected;
- realtime event identity fanout can share an immutable event representation;
- compiled event/sample access remains independent from realtime storage
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
        | one NodeStorage contains persistent + reserved reusable regions
        v
specialized whole-project LLVM
        |
        v
LLVM optimization / ORC
```

`ConfiguredGraph` remains free of physical storage choices. The first place
those choices become concrete is the compiler plan used to generate the whole
project kernel.
