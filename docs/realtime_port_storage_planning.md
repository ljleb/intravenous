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

Then choose among the legal representations using an explicit cost model:

```cpp
ConnectionImplementationPlan
 choose_connection_implementation(
     ConnectionStorageRequirements const&,
     ConnectionCostModel const&);
```

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

The physical scratch location need not always be the machine stack. Large graph
blocks may use one statically sized/preallocated executor scratch area. The
important properties are:

- no realtime heap allocation;
- offsets/lifetimes are known before execution;
- unrelated logical connections may reuse storage when their live intervals do
  not overlap.

## Realtime event ports need bounded time windows

Realtime event outputs must have a statically predictable temporal window just
like realtime sample outputs.

A realtime output callback must not be able to produce an event at an arbitrary
absolute time unrelated to the current invocation. Its legal output timestamps
must lie inside the finite window defined by the current block together with the
port's declared history and declared/corrected latency. In other words, realtime
event production is constrained by the same `current block + history + latency`
semantic extent used to make realtime sample access predictable.

The precise inclusive/exclusive coordinate convention should match the sample
port model, but the compiler must be able to prove a finite window for every
realtime event output invocation.

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

Time-window bounds determine lifetime, but not necessarily maximum event count.
The event planner may initially use an explicit capacity heuristic based on
value size/block/window information. If later workloads require stronger
bounds, node definitions may expose event-density/capacity hints. Capacity
policy remains a physical planning input, not connection semantics.

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
capacity/density estimate or bound
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
transient liveness + scratch allocation
        |
        v
persistent NodeStorage / lifecycle layout
        |
        v
specialized whole-project LLVM
        |
        v
LLVM optimization / ORC
```

`ConfiguredGraph` remains free of physical storage choices. The first place
those choices become concrete is the compiler plan used to generate the whole
project kernel.
