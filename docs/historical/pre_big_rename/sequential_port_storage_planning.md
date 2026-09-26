# Sequential Port Storage And Connection Planning

_Status: current design direction for whole-project Tick execution and sequential-consumption sample/event lowering._

Related documents:

- [DSP Execution And Storage Glossary](./dsp_execution_storage_glossary.md)
- [graph_jit_direction.md](./graph_jit_direction.md)
- [builder_lowering_pipeline_design.md](./builder_lowering_pipeline_design.md)
- [coverage_and_background_evaluation.md](./coverage_and_background_evaluation.md)
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

## Node-facing Tick APIs express logical access

A node author should use the same `tick_block()` port API regardless of the
chosen physical representation.

Conceptually:

```cpp
auto input = ctx.input<"input">();
auto output = ctx.output<"output">();

output[i] = process(input[i]);
```

must not mean "index a ring buffer." It means "access the logical port
at this sample position inside the statically legal Tick window."

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
generated Tick root must not substitute a first-call/run-once guard.

Truly request-sized caller data whose maximum size is not known at graph compile
time is not a legal backing strategy for Tick execution. It belongs to a background
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

## Port windows are semantically node-owned even when storage is shared

The authored history/latency declarations should be interpreted by an **as-if private
state** rule. For graph-revision semantics, each concrete node behaves as though its
port windows were ordinary state owned beside its nested `State`:

- a Sequential input owns the resolved values in its declared history window;
- a Tick output owns the values in its declared history window; and
- a Tick output owns its already-authored latency/future window.

This is an effect requirement, not a physical-allocation requirement. The steady
storage planner may satisfy several conceptual port states using one producer ring,
may let a consumer history view alias producer storage, and may eliminate dedicated
state entirely when the required values are directly addressable. Those are valid
optimizations only while they remain observationally equivalent to private node-owned
state across graph revisions.

A connection edit therefore changes future routing, not the past owned by a surviving
port. If `A.out -> C.in(history=H)` becomes `B.out -> C.in(history=H)` at position `P`,
then `C.in[P-H,P)` initially remains the resolved history C saw through the old graph.
The same applies to fan-in: historical composed values belong to the destination
input, not to whichever producer set happens to feed it in the new revision.

A graph-version transition may temporarily duplicate data that steady execution had
shared. For example, if old C history was merely a view into A's producer ring while
new steady C history can be a view into B's ring, the transition realization may copy
C's still-visible old history into a small carry. That copy is allowed to coexist with
other copies of the same physical source data; avoiding steady-state copies is the
optimization objective, not avoiding migration-time copies.

For a surviving port whose required extent changes, preserve the overlapping valid
semantic range. Shrinking history/latency may discard values that cease to be
observable. Growing it preserves the previously owned range and initializes the newly
exposed portion according to ordinary fresh-state semantics; do not fabricate old
consumer history from unrelated source persistence merely because source data happens
to exist.

The stable migration identity belongs to the concrete node endpoint, not the
connection or storage plan: user-instantiated node/module identity, virtual-node and
concrete-member path, port direction/ordinal, channel index or event stream, and a
state-role discriminator. Endpoint incidence classes, connection IDs, allocation
indices, compact/ring kind, capacity and offsets are generation-local physical facts.
Any physical plan that aliases/elides one of these conceptual state pieces must retain
cold metadata capable of reading that semantic window again during a later graph
transition.

## Partition overlapping endpoint subsets before choosing storage

Storage is not selected edge by edge and is not selected once for an entire authored
port. Fanout/fan-in selections may overlap only on subsets of channels, and those
subsets can participate in different connections elsewhere. The correctness unit is
therefore an **endpoint atom**: a maximal source or target subset with identical
connection incidence and identical semantic requirements.

Start from atomic payload elements (sample channels, or an event port/source unless
its routing semantics provide a finer partition) and partition them by incidence.
For example:

```text
output O = {a,b,c,d}

input I1 (Sequential)     <- {a,b}
input I2 (Random Access)  <- {b,c}
input I3 (Sequential)     <- {d}
```

induces source signatures:

```text
a : I1
b : I1 + I2
c : I2
d : I3
```

`b` must be separated because its uses differ from both neighbors. `a` and `d` may
have equivalent storage requirements even though they are not the same connection
subset; physical coalescing is a later choice. Conversely, if `{a,b}` participates
in exactly the same connections with the same conversion/timing facts, it may remain
one atom.

Perform the same partition from the target side. A target atom records the exact
incoming source atoms, conversion/composition, timing alignment, and access contract.
This matters for overlapping fan-in: direct channel placement can remain a set of
views, while arithmetic mixing into one target channel requires a derived
representation only for the affected target atom.

The planner therefore uses the sequence:

```text
configured connections
        ↓
atomic channel/event contributions
        ↓
source + target incidence partitions
        ↓
join correctness capabilities for every atom
        ↓
derive/deduplicate converted or composed representations
        ↓
physically coalesce equivalent storage where profitable
```

Partitioning is semantic and exact. Coalescing is an optimization. Do not merge atoms
first and then infer requirements from the union, because one Random Access or
retention requirement on one subset would unnecessarily promote unrelated channels.

### Storage requirements are capability joins, not one strongest enum

For each source atom `S`, derive an independent set of required capabilities from
its authored output properties and **all** outgoing uses:

```text
source_requirements(S) =
    intrinsic_output_requirements(S)
    UNION
    union(edge/use requirements for every outgoing contribution of S)
```

For each target atom `T`, independently derive the access/composition representation
required by all incoming source atoms:

```text
target_requirements(T) =
    authored_input_access(T)
    + composition/conversion/timing requirements(incoming(T))
```

Some capabilities subsume others. A prepared immutable addressable window can also
provide sequential slices for the same range. Other capabilities are orthogonal and
must coexist: a Tick/persisted source with a same-Tick Sequential consumer requires
a current Tick representation, capture/persistence staging, and canonical persisted
pages because current visibility and published snapshot visibility are different
contracts. These are logical capabilities, not necessarily separate payload buffers:
when geometry permits, the current Tick payload may itself be the allocator-managed
capture block later consumed/adopted by the page-publication path.

Useful monotone implications include:

```text
if output.retention == persisted:
    require canonical persisted-page storage
    require candidate/published version state and reader lifetime support

if output.production == Tick and output.retention == persisted:
    require capture-backed transfer from Tick lifetime into persisted-page publication

if output.production == Tick and any same-Tick Sequential consumer exists:
    require current Tick-readable representation

if output.production == Tock and output.retention == ephemeral
   and any Tick-time Sequential consumer exists:
    require prepared sequential window

if any Tick-time Random Access consumer requires an ephemeral Tock/replay result:
    require prepared immutable addressable window

if any background-only Random Access consumer requires an ephemeral Tock/replay result:
    require transaction-local addressable materialization

if Tick/ephemeral is unreproducible and any Random Access demand reaches it:
    reject the implicit connection; require authored persistence/recording
```

The corresponding baseline payload decisions per source atom/use are:

| Source atom / use | Minimum baseline representation |
| --- | --- |
| Tick/ephemeral -> same-Tick Sequential | current Tick representation |
| replayable Tick/ephemeral -> background Random Access | transaction-local addressable replay materialization |
| replayable Tick/ephemeral -> Tick-time Random Access | prepared immutable addressable replay window |
| unreproducible Tick/ephemeral -> any Random Access | disallowed implicitly; authored persistence or recorder required |
| Tick/persisted -> same-Tick Sequential | current Tick representation + capture-backed persistence staging + canonical persisted pages |
| Tick/persisted -> Random Access | canonical published persisted pages; capture staging is intrinsic to persistence, but the current capture is not a baseline read source |
| Tock/ephemeral -> Tick-time Sequential | prepared sequential window |
| Tock/ephemeral -> background Random Access | transaction-local addressable materialization |
| Tock/ephemeral -> Tick-time Random Access | prepared immutable addressable window |
| Tock/persisted -> Tick-time Sequential | canonical published persisted pages generated sufficiently ahead of playback |
| Tock/persisted -> Random Access | canonical published persisted pages |

For several outgoing uses, take the capability union of the applicable rows and then
remove payload representations subsumed by another requirement for the same atom/range.
For example, a prepared addressable window subsumes a prepared sequential-only window,
while current Tick visibility and a published persisted snapshot do not subsume one
another. This table describes logical capabilities; physical planning may alias the
current Tick payload with a capture block when the payload is already final under the
Tick history/latency contract and page/capture geometry permits it.

`RandomAccessInputConfig` alone does not state whether node code reads that input
from Tick, Tock, or both. GraphJit should use statically known callback access when
available. Until that fact is represented precisely, planning must conservatively
join the requirements of every callback context in which the input can legally be
read.

### Fanout joins requirements; fan-in may create derived representations

For ordinary fanout:

```text
Tick/persisted source
   +--> Sequential Tick consumer
   +--> Random Access consumer
```

requires current Tick visibility plus canonical persisted pages. Capture-backed
staging bridges the producer lifetime into page publication. The preliminary Random
Access contract reads only the callback-pinned published snapshot, while the
Sequential edge may consume the new current block immediately after the producer
executes. The Random Access edge therefore adds no same-Tick dependency in this
baseline implementation; only the Tick-to-Sequential edge orders the producer and
consumer. If layout permits, the producer's current payload and capture block may be
the same physical block, so the extra capabilities do not imply an extra copy.

For a Tock/ephemeral source:

```text
source
   +--> Sequential Tick consumer
   +--> Random Access Tick consumer
```

a prepared addressable window may satisfy both uses; there is no reason to allocate
a second sequential-only copy for the same atom/range.

Fan-in is different. If independent source channels merely fill distinct target
channels, the target can often remain a set of direct views. If several sources
arithmetically contribute to the same target channel, materialize the derived value
with the lifetime/access required by the target: current-block for same-Tick
Sequential use, transaction-local addressable for background Random Access, or
prepared immutable addressable for Tick-time Random Access. Event fan-in follows the
same lifetime rule but must additionally preserve deterministic event ordering.

A derived representation can be shared only when its source-atom set,
conversion/composition, temporal mapping, selected semantic/page snapshot, requested
range, and lifetime/access contract are equivalent. This prevents overlapping
connections from accidentally sharing a result computed for a different subset or
version.

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
- what temporal window is legal for Tick event production and sequential event consumption?

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

### Sequential port storage plans

There are three useful physical storage plans for an event stream or a sample
channel group:

| Storage plan | Invocation-local storage | `NodeStorage` | Copies caused by retention |
| --- | --- | --- | --- |
| transient | One fixed-capacity buffer in the generated root stack frame. | None for the stream. | None. |
| transient with persistent carry | One fixed-capacity working buffer in the generated root stack frame. | Exactly the history/latency tail that must cross root calls. | Restore the retained tail into the working buffer and commit the next retained tail back out. |
| full persistent | None is required merely to reconstruct the stream. | One fixed-capacity buffer covering the complete simultaneously-live window. | No root-boundary reconstruction copies; producer and compatible consumers address the persistent buffer directly. |

A shared physical-planning vocabulary can therefore begin with:

```cpp
enum class SequentialBufferStorageKind {
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

- exact source and target endpoint-atom inference now runs after latency
  compensation and before physical producer grouping. Sample channels are
  partitioned by complete connection/contribution/timing incidence; event ports
  remain whole-port atoms. Disconnected authored input and output elements are
  retained too, so intrinsic retention/requestability and target identity do not
  depend on current fan-in/fan-out. Each atom retains its joined
  current-Tick, capture, persisted-page, prepared-window, and transaction-local
  addressability capabilities. Port-granular coverage endpoints remain separate
  from these storage atoms. Background connection records retain their exact
  source/target atom ordinals, and existing node-facing Tick producer groups
  record the atoms they physically coalesce, while background
  page/materialization realization remains executor work;
- immutable indexed physical planning now converts those requirements into
  canonical persisted-page bindings, non-owning current-Tick views, prepared
  sequential/addressable windows, and transaction-local addressable
  representations. Direct sample channels and single-source exact-type events
  retain views; conversion and fan-in produce typed materialization templates.
  Equivalent derived templates share only when source atoms, selected input
  residences, transform, timing, target subset, history, and output residence
  match. A prepared addressable result also subsumes an otherwise-identical
  prepared sequential result, independent of configured connection order.
  Runtime range/page-version selection remains executor state;
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

- no audio-thread heap allocation;
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
`t + 1`; current built-in conversions never invent a later timestamp. Tick-window
validation nevertheless checks converted events at the point they are
emitted, so future conversion additions cannot silently escape the legal
window.

## Tick event ports need bounded time windows

Tick event outputs must have a statically predictable temporal window just
like Tick sample outputs.

A Tick output callback must not be able to produce an event at an arbitrary
absolute time unrelated to the current invocation. Its legal output timestamps
must lie inside the finite window defined by the current block together with the
port's declared history and declared/corrected latency. In other words, Tick
event production is constrained by the same `current block + history + latency`
semantic extent used to make sequential sample access predictable.

For a callback beginning at global sample index `B`, block size `N`, output
history `H`, and effective/corrected output latency `L`, the legal Tick event
output extent is the half-open interval:

```text
[max(0, B - H), B + N + L)
```

The compatibility runtime enforces this exact half-open convention. Lowering
may remove redundant checks when authored/generated accesses are statically
proved to stay inside the same extent.

The compatibility runtime should validate this constraint. The whole-project
JIT may then specialize it away when the authored access is statically valid.

Arbitrary `TimedEvent` insertion outside that window is not part of the Tick-output
contract.

## Sequential timing is independent of output production and retention

Input access, output production and output retention are independent; sample/event
payload properties remain a separate axis. The **target** declaration shape is:

```cpp
struct SequentialInputConfig { std::size_t history = 0; };
struct RandomAccessInputConfig {};
using InputAccessConfig =
    std::variant<SequentialInputConfig, RandomAccessInputConfig>;

struct TickOutputConfig {
    std::size_t history = 0;
    std::size_t latency = 0;
};
struct TockOutputConfig {};
using OutputProductionConfig =
    std::variant<TickOutputConfig, TockOutputConfig>;

enum class OutputRetention { ephemeral, persisted };

struct InputConfig {
    // Name/identity and sample/event payload properties omitted.
    InputAccessConfig access{SequentialInputConfig{}};
};
struct OutputConfig {
    // Name/identity and sample/event payload properties omitted.
    OutputProductionConfig production{TickOutputConfig{}};
    OutputRetention retention = OutputRetention::ephemeral;
};
```

The current checked-in C++ API uses these independent names directly.
`SequentialInputConfig` has the existing finite history contract; `TickOutputConfig`
has the existing history and latency authoring contract. A random-access input
can be consumed in either execution callback. A tick-produced output can satisfy
random-access demand through finalized persisted data or contextually replayable
computation; a tock-produced output can feed a sequential input if its data is
prepared off the audio thread. Production does not select the consumer's access.

`ephemeral` permits transaction-local prepared or page-backed materialization but
makes no lasting retention promise. `persisted` retains **all generated/finalized
covered data**: there is no automatic eviction for memory pressure, cache size,
age, invalidation, or lack of current readers. Coverage removal is the only semantic
reason to stop retaining that data; superseded physical versions can be reclaimed
after readers unpin them. Memory growth is the graph author's retention choice.

`InputConfig` / `OutputConfig` independently carry sample/event payload properties
and the above access/production/retention contracts. Static concrete node types
have constexpr port schemas. Port history and latency are not duplicated in
sample or event payload properties. The same facts survive `ConfiguredGraph`
reflection and serialization. Generic input/output mode-conversion helpers may
not invent a production callback or input access from the opposite declaration.

For scheduling/storage purposes, a node may have temporal dependencies if it has
`NodeState`, sequential history, output latency or an independent internal latency.
An explicitly replayable `tick()` node has none of those dependencies, no
random-access input, and meets its separate deterministic/side-effect-free trait
contract. GraphJit reuses its existing generated LLVM-imported `tick_block()`
wrapper and proves upstream availability for every background replay path.

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

Exceeding the declared maximum is outside the Tick producer contract and has
implementation-defined behavior. A particular implementation may drop excess
events and count them, but callers must not depend on that policy. It must never
grow a buffer or allocate memory on the audio thread.

This sizing rate belongs to the event **output payload properties**, not to
`TickOutputConfig`: history/latency define *when* an output may author data,
while `max_events_per_index` lets GraphJit determine how much static event
storage to reserve for the selected temporal representation.

## Random-access ports use explicit sparse coverage

`TockOutputConfig` publishes exact finite `IndexedCoverage`; random-access demand
may also use finalized published `tick/persisted` data or a contextually replayable
tick output. Outside coverage, a random-access node read is invalid. Coverage and
exact semantic changed regions remain distinct from aligned physical page domains.

A persisted candidate page is computed for its complete covered domain before the
candidate publishes. Tick/persisted and Tock/persisted outputs use the same canonical
persisted-page store; only their production/finalization paths differ. Invalidation
never deletes the existing readable published page. An ephemeral Tock output directly
feeding a background random-access input uses a transaction-local addressable
materialization. If the same ephemeral result must be read by Random Access during
Tick execution, background work must prepare an immutable addressable window before
the callback; that preparation is not persisted output data.

**The only implicit-storage connection that is forbidden** is an unreproducible
tick/ephemeral source directly feeding random-access demand, whether the input is
used by tick or tock. It requires an authored recording node that selects its own
retention/lifetime policy. All other source/input combinations are access-compatible,
subject to dependency availability, coverage, and execution scheduling. A tile
retains per-source channel capabilities and does not create a producer or recorder.

`tock_coverage()` and its propagation callbacks are **never executed on the audio
thread**. For persisted data, the preliminary implementation pins the selected
published page snapshot at the Tick callback boundary. Both Sequential page playback
and Tick-time Random Access use that immutable snapshot; pending candidate pages and
newly sealed Tick capture blocks are invisible until a successor page version is
published and a later callback selects it. An existing stale or invalidated published
page is read as-is, while a genuinely missing Sequential page produces that input's
own `neutral_value`. Playback does not block or synchronously generate missing pages.

Tick capture is the important cross-thread lifetime bridge. The same allocator-managed
capture-block infrastructure can serve both an explicit recording bridge and
Tick/persisted output staging. A recording bridge consumes ordinary sequential data
and exposes retained/background-computable output according to its authored policy;
a Tick/persisted producer uses capture to move newly finalized Tick data toward the
canonical persisted-page store without allocating on the audio thread.

When layout permits, the producer may write directly into a pre-provisioned capture
block; otherwise the generated path performs a bounded copy at the production/finalization
point. Capture does not wait for the end of the root Tick callback. Each capture
record carries at least:

```text
CaptureSequence
OutputPortId
GlobalBlockPosition
payload block
```

`CaptureSequence` is monotonically increasing insertion order in the executor's
shared Tick-capture log. `OutputPortId` identifies the capture-backed output whose
value/coverage is affected, and `GlobalBlockPosition` identifies where that change
belongs. Global positions need not increase with sequence: seeking during playback
may append a new capture for an earlier position, and consecutive captures may
belong to different outputs.

Capture capacity is slab-backed and dynamically extensible; the effective number of
recent/pending blocks is determined by allocator supply and background progress, not
by a fixed guessed duration such as one second. The audio thread only consumes
already-provisioned free blocks. A separate **capture allocator** maintains a target
free-block reserve by allocating reasonably sized slabs independently of background
evaluation. Slow background work therefore increases the sealed-but-unpublished
backlog rather than overflowing a compiler-planned staging ring.

A block acquired, written, or made readable during one root `tick_block()` callback
is not returned to the audio-thread free pool during that callback. Background work
may mark it reclaimable, but actual free-pool reuse is handed off at a callback
boundary after all audio-thread views from that epoch are dead. This rule permits
capture blocks to be shared safely by persistence and, in a later optimization,
recent same-Tick Random Access.

The background worker snapshots a fixed contiguous **capture-sequence prefix** at the
start of each propagation/tock pass. Contiguous here refers only to insertion
sequence; the selected records may cover arbitrary ports and nonmonotonic global
positions. Captures published after the snapshot cutoff are excluded from the
running pass and belong to a later pass.

The selected records are coalesced into exact changed coverage keyed by output port
and seed the normal forward-coverage propagation machinery. Reverse planning and
`tock_coverage()` then run normally for all affected nodes and page domains. The
background evaluation transaction builds candidate persisted pages and atomically
publishes one new page version. Capture insertion itself is **not** page
publication and does not advance the page version.

The canonical persisted-page store owns the published representation. A candidate
may copy from capture blocks or, when physical layout/ownership permits, adopt their
payload without changing the page-store abstraction. If a candidate copies, the
capture block becomes reclaimable once no background ownership remains; if the page
store adopts the payload, ownership transfers and that physical block is no longer a
free capture block until the published page version itself can release it. In either
case, a block that was visible during the current audio callback cannot return to the
audio-thread free pool until a callback boundary. If work is cancelled or rejected
as stale, the processed capture frontier does not advance and the corresponding
blocks remain available for a later transaction.

The preliminary Random Access implementation does not search these recent capture
blocks. A future optional optimization may treat sealed Tick/persisted captures newer
than the pinned published snapshot as a recent overlay, allowing a later Tick node
to Random-Access newly finalized data in the same callback. That requires an explicit
same-Tick producer dependency plus a lookup branch between recent blocks and
published pages (or an ordered two-source merge for events). It must not be enabled
implicitly until those visibility/version rules are implemented and tested.

Changing the root block size is a quiescent physical-layout transition, not a
semantic invalidation. Persisted output values are losslessly
repartitioned as needed, a replacement GraphJit generation receives the new
canonical layout, and publication switches only after migration completes.
Semantic versioning and physical layout generation remain distinct.

Persistence does not alter `tick_block()`'s legal history/latency writes: only
finalized positions acquire the retention obligation. Those finalized published
positions may be read by random-access inputs **directly**, without forcing a
recording bridge or a tock implementation. An explicit recorder remains necessary
for an unreproducible ephemeral tick source.

Persistent stored sample payloads may be dense or coverage-packed. Stored event
payloads are packed ordered events; event fan-in order is deterministic by
absolute sample index, stable source/connection ordinal, then producer-local
order. Combined live event-buffer capacities must account for all incoming
`max_events_per_index` bounds.

See [coverage_and_background_evaluation.md](./coverage_and_background_evaluation.md) for the normative coverage propagation,
background evaluation, and publication semantics.

## Event storage planning mirrors sample storage planning where possible

Once Tick event windows are bounded, event connection storage can also be
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
mask. Every Tick event representation must have such a finite compile-time
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
sums their rates for the merged representation. Tick event producers are
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
conversion/materialization fanout does not duplicate the counter. Audio-thread
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
- Tick event production outside the legal window is rejected;
- every event buffer capacity is derived from producer
  `max_events_per_index` and its exact simultaneously-live temporal span;
- unrepresentable Tick-event capacities fail planning and never fall back to a
  runtime allocation;
- carry and full-persistent candidates preserve the same event semantics while
  exposing their different copy counts to policy;
- feedback capacity is derived from delayed live span rather than multiplying
  one invocation capacity by a callback count;
- Tick event identity fanout can share an immutable event representation;
- random-access event/sample consumption remains independent from sequential-consumption storage
  planning.

## Graph-revision transition planning precedes optimization

A new logical graph revision may have two physical realizations:

```text
old executable
      |
      | safe splice at absolute position P
      v
transition realization of new graph
      |
      | last inherited transition-only port state expires
      v
steady realization of the same new graph
```

The transition realization is required only when the final steady plan cannot itself
represent inherited node-owned state. It may add compact carries, composed-history
materializations, or other bounded temporary storage. Those requirements have a
finite semantic range for ordinary history/latency and therefore an absolute expiry
position. GraphExecutor may translate that to a known block count for a fixed block
size, but the semantic handoff is at the first legal root callback boundary at or
after the expiry position.

GraphJit should plan/compile both realizations during the original graph rebuild when
both are needed. The second handoff is activation of already compiled code, not a
second asynchronous compilation. State that has evolved while the transition form was
active is reconciled into the steady realization at that handoff. If the steady
representation can directly absorb all inherited state, only the steady realization
is necessary. If another graph edit supersedes the transition before its expiry, the
next rebuild uses the currently active transition realization as its old semantic
state source and discards the obsolete pending steady form.

This transition-correctness layer must land **before further storage/code
optimization**. It is deliberately independent of later `NodeStorage` simplification:
the first implementation may use the current raw-region/carry/ring machinery, as long
as it can expose each semantic port-state view during reconciliation and preserve the
required range.

## Compiler pipeline placement

The whole-project compiler order should make the boundary explicit:

```text
ConfiguredGraph logical connections
        |
        v
schedule / dependency / SCC analysis
        |
        v
history / latency / Tick-event-window analysis
        |
        v
derive stable concrete-node port-state identities/ranges
        |
        v
derive connection storage requirements
        |
        v
choose steady sample/event implementation plans
        |
        v
reconcile old semantic state realizations
        |
        +--> direct migration into steady plan, when sufficient
        |
        `--> transition realization + finite expiry horizon, when required
        |
        v
transient liveness + reusable-region allocation
        |
        v
root declaration / canonical NodeLayout planning for required realization(s)
        |
        | NodeStorage contains only cross-call state;
        | generated root owns fixed transient arenas
        v
specialized whole-project LLVM realization(s)
        |
        v
LLVM optimization / ORC
```

`ConfiguredGraph` remains free of physical storage choices. The first place
those choices become concrete is the compiler plan used to generate the whole
project kernel.
