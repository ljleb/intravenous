# DSP Execution And Storage Glossary

_Status: normative terminology for current DSP, GraphJit, coverage, storage, and executor design documents._


> **Planned terminology change:**
> [Random-Access Port Data, Audio Value Types, And Input Contract Direction](./random_access_port_data_direction.md)
> introduces Region versus Coverage random-access sample forms and allows Coverage
> Regions to overlap. Where this glossary describes Coverage as only a set/canonical
> union of positions, the planned Region/Coverage contract takes precedence for the
> migration target.

This glossary names independent architectural properties directly. It intentionally
avoids using one broad adjective to imply several different facts at once.

The governing rule is:

> Use the narrowest term that states the property the sentence actually depends on.
> If a statement depends on several independent properties, state several properties.
> If no qualifier is needed, omit it.

The port model has three independent authored axes:

| Axis | Terms | Meaning |
| --- | --- | --- |
| input access | **Sequential**, **Random Access** | how an input consumes values |
| output production | **Tick**, **Tock** | how an output's values are produced |
| output retention | **Ephemeral**, **Persisted** | whether finalized/generated values carry a retention obligation |

None of these terms implies either of the other axes.

## Input access

### Sequential input / sequential consumption

A **Sequential** input consumes a bounded ordered window associated with ordinary
block execution. Its configured realization may require finite history resolved through
port constraints.

Use **sequential consumption** when the sentence is about the consumer behavior,
not the output that happens to feed it.

Sequential consumption does not imply Tick production. A Tock output may feed a
Sequential input when its data has been materialized before the consuming pass.

### Random-access input / random access

A **Random Access** input may address arbitrary global positions inside its exact
available coverage.

Use **random-access** as an adjective and **random access** as a noun phrase.
Random access does not imply Tock production or persisted retention. It may consume,
for example, persisted Tick data, persisted or ephemeral Tock data, or a replayed
Tick result when the required source data is reproducible.

## Output production

### Tick output / Tick production

A **Tick** output uses the ordinary Tick production contract represented by
`TickOutputConfig` and the node's generated/imported `tick_block()` implementation.
In ordinary project execution, Tick production participates in the same-Tick
schedule.

Tick is a production contract, not a thread name. An eligible Tick implementation
may also be invoked during background replay.

When the callback/API distinction matters, use **Tick** or `tick_block()` directly.
When only the ordering behavior matters, describe the value as **sequentially
produced**.

### Tock output / Tock production

A **Tock** output uses `TockOutputConfig` and is produced through demand-driven
background evaluation using `tock_coverage()`.

Tock is a production contract, not a synonym for random access or persistence.
A Tock output may be ephemeral or persisted, and may feed either Sequential or
Random Access inputs.

### Scalar callback / batch callback

A **scalar callback** operates on one configured concrete node instance. A **batch
callback** operates on one or more compatible instances of the same concrete node
implementation and resolved callback-facing realization. Each lane retains its own
node/configuration, state, ports, and, for background work, Coverage.

Batching is an execution optimization, not a different DSP semantic contract. A
native batch callback must be lane-wise equivalent to the corresponding normalized
scalar operation, and a batch of size one is always legal. GraphJit may therefore
split or scalarize a batch candidate when dependency scheduling or the cost model
prefers it.

The planned callback/range API and trait normalization rules are specified in
[Batched Node Callback Direction](./batched_node_callbacks_direction.md).

## Output retention

### Ephemeral output

An **ephemeral** output has no authored retention obligation after the evaluation
or materialized use that requires its value.

Ephemeral does not mean stack-allocated, contiguous, unpaged, or recomputed for
every consumer. Background evaluation may create a transaction-local materialization
that several consumers share.

### Persisted output

A **persisted** output has the runtime retention guarantee defined by
`OutputRetention::persisted`: generated/finalized values remain retained while they
remain in output coverage, subject to version replacement and reader lifetime.

**Persisted does not mean serialized to disk.** Backing storage may be memory,
file-backed storage, mmap, or another representation that preserves the retention
contract.

Use **persistent** separately for ordinary storage lifetime or placement, such as
persistent `NodeStorage`, a persistent ring, or a persistent compiler service.
Do not use **persistent** as a synonym for the authored `persisted` output-retention
contract.

## Port value type, size and pacing

### Registered continuous value type

A port's **registered continuous value type** determines the representation and
storage semantics of one transported dense value. The registry is closed/explicit in
the same architectural sense as the event- and channel-type registries. The planned
initial set includes scalar `Sample`, scalar `GlobalIndex`, and `FFTBlock`.

A static port schema may declare one concrete value type or a closed finite set of
supported value types. A multi-type port's selected member is resolved by graph
constraints before storage planning/LLVM lowering; it is not a per-value runtime tag.
Direct connections initially require equal resolved value types. Explicit DSP or
conversion nodes perform representation changes unless a future design deliberately
adds an implicit conversion.

### GlobalIndex

`GlobalIndex` is a scalar fixed-point value representing a fractional coordinate in
the global sample-index domain. It retains the full integer global-index width and adds
64 fractional bits. A `GlobalIndex` transported at integer graph position `p` is still
data located at `p`; its numeric value may point somewhere else and is interpreted as
a coordinate only by nodes that explicitly use it that way, such as a resampler.

### Port value size

A port value's **size** is the registered value-type-specific fixed-rank extent of one
transported value in the active realization. Scalar `Sample` has no dynamic extent;
an FFT block has one dimension whose resolved value is its frequency-value count.
Size is structural realization data and must be resolved before GraphJit chooses
storage or lowers code.

Do not use **size** to mean the number of transported values in one `tick_block()`
invocation; that quantity is the port's block size.

### Port pace

A port's **pace** is the exact relative quantity of transported values consumed or
produced per local logical node step. Different ports of one node may have different
paces. Pace establishes execution/index-domain relationships; it does not perform
resampling or otherwise compute new signal values.

Pace is a graph-resolved realization fact contributed through port constraints and
connections. Exact integer/rational relationships should be preserved so different
callback subdivision choices cannot accumulate timing/index drift.

### Per-port block size

A port's **block size** is the number of transported values presented to or produced by
that port during one `tick_block()` invocation. Under heterogeneous pacing there is no
single node-wide block size. The specialized Tick-block context exposes the resolved
count per port.

For an FFT-block port, `size() == 2048` and `block_size() == 4` means four transported
FFT blocks, each containing 2048 frequency values.

A Sequential input's **history-inclusive block** is the contiguous view consisting of
its resolved history immediately followed by the current block. The provisional API
name is `block_extended()`. It is a Sequential view, not a Region or Coverage;
GraphJit may alias existing storage or materialize the finite view when contiguity
requires it.

### Effective local sample rate

A node's **effective local sample rate** is the sample rate of its resolved execution
rate domain, exposed after pace analysis to declaration/initialization/execution
contexts. It is not a constant available during `constrain_ports()` because the
constraint solve determines the domain itself. A node in a 2x oversampled region of a
48 kHz project observes 96 kHz.

Use **pace** for relative per-port transport quantities and **effective local sample
rate** for the physical/audio rate seen by the node. They are related by graph
analysis but are not synonyms.

## Graph relations and execution structures

The design uses three distinct graph relations. They must not be collapsed into one
"execution domain" graph.

### Semantic dependency graph / semantic SCC

The **semantic dependency graph** contains the logical sample/event dependencies
needed to reason about semantic cycles. It is independent of output production,
input access, and scheduling mechanism. Explicit detach/feedback edges remain
semantic dependencies even when they are removed from same-Tick scheduling.

A **semantic SCC** is a strongly connected component of that relation.

### Same-Tick scheduling graph

The **same-Tick scheduling graph** contains dependencies that must order ordinary
Tick production within the same execution slice. In the current port model,
Tick-to-Sequential contributions are the ordinary same-Tick scheduling dependencies.

Use **same-Tick dependency**, **Tick schedule**, or **same-Tick scheduling graph**
when scheduling order is the property that matters. Under the preliminary
published-snapshot Random Access implementation, a Tick-to-Random-Access connection
does **not** create a same-Tick dependency: the consumer sees its callback-pinned
published snapshot available at callback entry, not the producer's current block. A future recent-capture
overlay that promises same-Tick Random Access visibility would add such a dependency
explicitly.

### Background evaluation DAG

The **background evaluation DAG** contains the dependencies required to satisfy
coverage demand outside the ordinary same-Tick schedule. It can contain authored
Tock execution, generated Tick replay, and dependencies traversed while proving or
executing replay. Valid persisted outputs may terminate traversal as stored
boundaries.

The background evaluation DAG is not the semantic dependency graph and is not a
second audio-thread scheduler.

### Background evaluation component

A **background evaluation component** is a statically planned component of the
background evaluation DAG with retained forward/reverse/evaluation ordering and
port metadata.

## Scheduling contexts

### Audio thread / audio-thread execution

Use **audio thread** only when the scheduling or hard execution constraint actually
matters: no blocking, no request-sized allocation, pre-provisioned Tick recording,
or safe-boundary publication.

Do not use Tick as a synonym for the audio thread. Tick callbacks may also be reused
for background replay.

### Background worker / background evaluation

**Background evaluation** is the off-audio-thread work that propagates coverage
changes and requirements, executes Tock or replay work, materializes required data,
and publishes completed versions.

A **background worker** is a runtime worker that performs such work. The semantic
term is background evaluation; the number or implementation of worker threads is a
runtime choice unless a document states otherwise.

### Async capacity manager

The **async capacity manager** is non-app-module runtime infrastructure that provisions
and reclaims stable fixed-capacity blocks off the realtime path. It is independent of
background-evaluation progress: a slow background worker increases pending ownership
rather than changing the producer's no-allocation rule.

Each producer has a **producer reserve** of ready blocks. The producer supplies:

```text
C = maximum producer burst that must fit without new provisioning
L = low ready-capacity watermark
H = refill target
```

`C` is a structural bound used to derive/validate reserve policy. `L` covers tolerated
manager detection/scheduling delay plus safety margin and `H` supplies refill
headroom/hysteresis. Allocation/slab granularity is manager-owned implementation detail,
not a semantic per-producer parameter. The intended relationship is `C << L < H`.

The manager may keep allocation/release accounting private. Queue users do not require
one shared atomic logical size. Completed blocks return from the single background
worker through a released-block stream and are reclaimed/reassigned by the manager.

### Graph executor / realtime and background actors

`GraphExecutor` is the execution app module. It owns complete prepared `ExecutionGeneration` objects and application-facing staging/coordination.

Its internal `RealtimeExecutor` owns realtime mutable execution state, active/pending
realtime generations, pass-boundary activation, and realtime producer endpoints.

Its internal `BackgroundExecutor` owns its worker thread, background mutable evaluation
state, independently queued producer inputs, persisted-data computation/publication,
and immutable persisted-state versions returned to realtime.

The two actors share no mutable state object. Cross-boundary data is immutable after
publication or ownership-transferred by pointer. Their handoffs are internal runtime
communication, not app-module event propagation.

## Coverage and change propagation

### Global position / range

Use **global position** or **global range** for the coordinate system in which
sample/event data and coverage are addressed. Use **sample index** when an actual
integer sample coordinate is specifically meant.

### Coverage

**Coverage** is the exact set of global positions for which an output semantically
exists and may legally be requested. `Coverage` is the current C++ type name
for this concept.

Coverage is independent of page boundaries and storage placement.

### Requested coverage

**Requested coverage** is the covered output region that a consumer or internal
completion operation requires to be available or computed.

### Changed region

A **changed region** is a sound region superset whose semantic value or coverage may
have changed. It may be exact, but exactness is an optimization rather than a semantic
requirement. Every actually affected position must be included. Stored-page
invalidation must not widen the already-reported downstream semantic change merely
because a whole page becomes locally invalid.

### Invalidation

**Invalidation** marks previously valid data as not current for a target semantic
version. It does not imply immediate destruction of the currently published readable
representation.

Use **semantic invalidation** when distinguishing semantic staleness from storage
layout or allocation changes.

### Forward coverage propagation

**Forward coverage propagation** maps current input coverage, changed-input
may-change regions, and semantic/configuration change causes to exact output coverage
and a sound may-change superset for output values. It may conservatively over-report
affected positions but must never under-report them.

For authored Tock nodes this is provided by `propagate_forward_coverage()`; eligible
Tick replay uses compiler-generated propagation where the mapping is mechanically
known and authored background propagation where a non-pointwise Random Access
dependency requires it.

### Reverse coverage propagation

**Reverse coverage propagation** maps requested output coverage to required input
coverage. It is a sound may-read upper bound and is value-blind under the current
design; conservative over-request is legal while under-request is a correctness error.

For authored Tock nodes this is provided by `propagate_reverse_coverage()`; eligible
Tick replay uses compiler-generated propagation.

### Replay

**Replay** is background recomputation of an eligible Tick implementation using its
already generated/imported `tick_block()` wrapper. Intrinsic replayability is a node
trait; contextual replayability additionally depends on the whole upstream graph and
available data. Random Access inputs do not inherently prevent replayability: they are
dependencies that must be resolved to immutable addressable views before the replay
invocation. The audio-thread Tick path likewise sees only already-prepared Random
Access views and never runs Tock or request-driven materialization.

### Persisted Tick boundary

A **persisted Tick boundary** is finalized Tick-produced persisted data that can
satisfy downstream random-access demand without replaying its live producer. It is a
terminal stored boundary for the covered positions available in the selected
version.

## Background evaluation and publication

### Background evaluation transaction

A **background evaluation transaction** is one coherent unit of invalidation-root
and demand-root collection, forward/reverse propagation, and required Tock/replay
evaluation against one coherent semantic/page-version view. When persisted state is
changed, the same transaction completes and publishes the corresponding candidate.

Forward/reverse propagation produces **prepared coverage**. It is not authoritative
until final commit promotes that coverage together with the successor persisted-state/
page snapshot and the domain-specific release of selected producer-queue prefixes.
Failure, cancellation, or stale-base rejection promotes none of that committed state.

This term describes atomic evaluation/publication semantics; it does not imply a
database transaction implementation.

A transaction may batch many roots and many disjoint regions. **Batch** is therefore
not a separate architectural domain; it describes work coalesced into one
transaction or callback invocation.

### Semantic version

A **semantic version** identifies the graph/configuration/resource/sample-rate
semantics under which values are meaningful. Do not qualify this as a coverage or
storage version: it is intentionally broader.

### Page version

A **page version** identifies one immutable published page-backed output view
produced by a completed background evaluation transaction. Use the singular phrase **page
version** and the conceptual pair `(semantic_version, page_version)`.

### Candidate / published

A **candidate** is a not-yet-published successor being completed under a target
semantic/page version. **Published** data is the immutable version externally or
sequentially readable as the current completed result.

Incomplete persisted candidates are not exposed as partially valid published
outputs.

### Reader pin

A **reader pin** keeps an immutable published representation alive while a reader
uses it. Superseded storage versions may be reclaimed after their pins disappear.
For Tick readers, pin acquisition/release and final retired-version reclamation must
be audio safe: no allocation, blocking, unbounded locking, or synchronous final-owner
destruction occurs on the audio thread.

## Storage representations and lifetimes

### `NodeStorage`

`NodeStorage` is the canonical fixed-layout **audio-thread** storage owned for an
executable realization. A logical graph revision may briefly have transition and
steady realizations, each with its own canonical `NodeStorage`. It contains realtime
`State` and compiler-selected persistent regions used by generated audio-thread
execution, packed for locality and low callback overhead.

It is not the owner of `TockState`, background workspaces, dynamically sized
persisted-output pages, or recording capture backlogs. `TockState` is separately
owned background acceleration state and may use dynamic allocation.

### Persisted page / persisted-page store

A **persisted page** is the canonical retained/versioned storage unit for persisted
sample or event output data. **All persisted outputs use the same persisted-page
store abstraction regardless of whether their producer is Tick or Tock.** Production
mode changes how pages are filled; it does not create another retained-data read
path.

Page validity is version-specific. An older readable persisted page may remain pinned
while a successor candidate is incomplete or invalid. Backing storage may still be
RAM, mmap/file-backed storage, immutable chunks, compression, or another page-store
backend, but those are implementations of the same persisted-page abstraction rather
than distinct semantic representations.

Do not call an ephemeral intermediate a persisted page.

### Transaction-local materialization

A **transaction-local materialization** is an ephemeral concrete representation
created for one background evaluation transaction and reusable by consumers within
that transaction.

For a Tock/ephemeral or replayed Tick/ephemeral result feeding a background-only
Random Access consumer, the preliminary implementation may use a
**transaction-local page-backed materialization** so the consumer receives the
required addressable representation. If Random Access occurs during Tick execution,
the addressable representation must instead be materialized and selected before the
callback. Replayable Tick work may avoid or fuse materialization where the consumer
contract permits it. A transaction-local page-backed materialization is not a
persisted page and carries no retention guarantee beyond the transaction/readers
that require it.

### Materialized sequential data / materialized addressable window

**Materialized sequential data** is background-produced data made available before a
future Sequential consumer runs. It is a delivery/materialization role, not an
output-retention mode; the source may be ephemeral or persisted.

For a Tock/ephemeral source, a bounded rolling **materialized sequential window** may be
sufficient when all Tick-time consumers are Sequential. If any Tick-time consumer
requires Random Access, the materialization must instead provide an immutable
**materialized addressable window**. The latter can also provide sequential slices, so
one addressable materialization may satisfy both uses for the same source subset.
Persisted sources normally need no separate playback copy: Sequential consumers can
view the appropriate pinned persisted pages directly.

### Tick materialization snapshot

A **`TickMaterializationSnapshot`** is the immutable callback-lifetime owner/view set
for Tick-visible ephemeral advance materialization. It is selected at the root
boundary and remains alive through every callback reader. It is not a published
persisted-page version, has no persisted retention guarantee, and must not be called a
"published materialization." The concrete snapshot records its compiled generation
and source persisted-page version. Background packages it with that exact immutable
page root in one latest-version mailbox owner; realtime adopts only a compatible pair
at a pass boundary. Promotion and adoption relink already-built owners without
allocation, and explicit non-audio reclamation destroys returned pair owners.

### Current Tick representation

A **current Tick representation** is the mutable/current-block representation written
by Tick production and read by same-Tick Sequential consumers after the required
same-Tick dependency has executed. It is not a published persisted snapshot and is
never the baseline backing for a Random Access input.

### Producer reserve / pending queue / produced block chain / pinned prefix

A **producer reserve** is the producer-facing set/chain of already allocated writable
blocks assigned by the async capacity manager. Realtime code only acquires from this
reserve; it does not resize or provision it.

A **pending queue** is the background-facing ordered chain of already published work
for one producer. Producer reserve and pending queue are two ownership roles of the
same logical SPSC transport; they need not be one public container object.

Backing block capacities are powers of two. A producer may use a block directly for
final sample/event payload rather than first writing another retained buffer and copying
it into transport storage.

A **produced block chain** is one or more fully initialized producer-private blocks
linked together before publication. The producer passes both first and last where
convenient. Publishing the chain is one cheap pointer operation; individual entries
inside the private chain do not require atomic publication.

A **pinned queue prefix** is the finite `(first,last)` block range selected by
`BackgroundExecutor` before one background workload begins. Later insertion may link
new blocks after `last`, but the selected workload still ends at the remembered
`last`; background execution does not discover more work while processing the prefix.

Different producer queues are pinned independently. There is deliberately no atomic
relationship between the prefixes selected from different queues. An item published
concurrently with workload selection may belong to either the current or a later
background pass. If a future feature requires atomic visibility across queues, that
requirement must be designed explicitly rather than added implicitly to the queue
primitive.

For a Tick/persisted recording output, invocation-local disposition distinguishes
three cases: an untouched output publishes nothing and preserves the prior RAM recording;
an ordinarily written output publishes payload data that overwrites the addressed
range; `write_void()` publishes an explicit authoritative erase for that range. This
is fixed recording semantics, not recorder policy. An explicitly authored empty
Tick/persisted event block remains an ordinary authoritative empty event payload rather
than a recorder void; merely omitting event output authoring leaves it untouched.
Disposition covers the entire addressed Tick callback block. A scalar `tick()` block
contains one sample. Output history/latency may widen the mutable storage window, but
does not widen the recorded overwrite or erase range. Tock/persisted output instead
authors exact arbitrary coverage and does not use Tick recording disposition.

A **queue-capacity failure** means a producer could not obtain already-provisioned
blocks for work that semantically had to be published. Later replenishment cannot
recreate the missed work. Tick/persisted therefore treats such a miss as a broken
retention guarantee, and recording treats it as an incomplete recording; resource
exhaustion never authorizes intentional loss.

Block ownership follows:

```text
AsyncCapacityManager -> ProducerReserve -> producer-private -> PendingQueue
                     -> released-block stream -> AsyncCapacityManager
```

The background consumer does not splice released blocks directly into a realtime
producer reserve. It releases committed blocks to non-realtime reclamation, and the
capacity manager owns recycling/reassignment.

### Graph-generation cutover / closed producer queue

A **graph-generation cutover** is the authoritative realtime pass-boundary transition
from one complete prepared `ExecutionGeneration` to the next. Both internal actor
realizations and every required route/reserve/migration resource are prepared before
the successor pointer becomes pending. The realtime boundary publishes final
old-generation chains, publishes the already-prepared successor generation pointer to
`BackgroundExecutor`, and swaps the active realtime realization.

A **closed producer queue** is an old-generation queue after that cutover. No producer
can append further items to it, so it has a finite tail even though the background
worker may not have drained it yet. The worker may finish old-generation selected work
and drain closed old-generation queues before applying the prepared background
migration. No global atomic snapshot across those queues is implied.

Queued data is always interpreted by the generation that produced it. Stable logical
identity controls whether retained recording/persisted state survives the transition;
generation-local compiled indices do not. A disappeared producer stops future writes
without erasing a surviving destination, while a disappeared destination is retired
after remaining old-generation work is interpreted under the old graph.

A **generation-compatible persisted-state version** is an immutable background
publication tagged/bound to the graph generation whose realtime bindings may consume
it. Background publishes these through a latest-version mailbox: if several compatible
versions are produced before realtime consumes one, only the newest pending version
must be retained. A late final old-generation version is transition input for
background migration, not a version that can become active in a newer realtime
generation.

### Published-snapshot Random Access

The preliminary Random Access implementation reads one immutable selected/pinned
published representation. For persisted outputs that representation is the
persisted-page snapshot. Pending candidates, current Tick buffers, and pending realtime-produced queue blocks do not extend Random Access coverage or visibility.
A Tick callback therefore cannot observe a newly produced Tick/persisted block until
a successor page version containing it has been published and a later callback/read
context selects that version.

A future optimization may overlay a bounded recent set of newly produced queue
payloads on top of the published persisted-page snapshot so a later node in the
**same Tick** can Random-Access newly finalized data. That optimization requires a
same-Tick producer-to-consumer dependency, a source-selection branch (published page
versus recent queued payload; ordered merge for events), and pass-boundary-safe
ownership/reclamation. It is deliberately not part of the preliminary implementation.

### Port atom / incidence partition

A **port atom** is a maximal source or target subset whose members have identical
connection incidence and therefore identical correctness requirements before
storage coalescing. Sample channels are the natural initial source elements; event
ports may remain whole-port atoms unless routing semantics introduce a finer split.

An **incidence partition** splits overlapping fan-in/fan-out selections into port
atoms before storage is chosen. Storage requirements are then joined across every
use of each atom. Equivalent atoms may later share a storage policy or allocation;
partitioning for correctness and coalescing for efficiency are separate stages.

### Node-owned port state

**Node-owned port state** is the semantic history/latency state of a surviving
concrete node's ports across graph revisions. A Sequential input owns its resolved
history; a Tick output owns its resolved history and latency/future window. This is
an **as-if private state** rule: storage lowering may alias or share the data with
producer timelines or other representations, but graph replacement must preserve the
same observable state that private node-local storage would have preserved.

Connection topology is not the identity of this state. Rewiring changes future
routing while still-visible destination history remains unchanged until it ages out.
A stable identity is rooted in user-instance/concrete-node/virtual-member and
port/channel-or-event-stream identity plus the state role. Storage representation,
capacity, offset, incidence partition and ordinary connection identity are not part of
that semantic identity.

### Transition realization / steady realization

A **transition realization** is a temporary compiled realization of a new
logical graph revision that contains extra bounded state needed to preserve inherited
node-owned port state which the steady representation cannot directly expose.
A **steady realization** is the compiled realization used after all transition-only
state has expired.

Both represent the same logical graph revision. GraphJit should compile both in the
original rebuild when both are necessary; `RealtimeExecutor` activates the transition
form at the splice and later reconciles currently evolved state into the already-
compiled steady form at a safe root callback boundary. Expiry is defined by absolute
semantic positions/ranges, even if a fixed-block implementation also precomputes the
equivalent callback count. A newer graph revision that arrives before expiry starts
from the active transition realization, not from its pending steady successor.

## Source availability

### Live source

Use **live source** only when the relevant property is that historical values are
not inherently reproducible from retained data. An unreproducible ephemeral Tick
source needs authored persistence or an explicit recording node before it can
satisfy historical Random Access demand.

Do not use **live** as a synonym for Tick, Sequential, or audio-thread execution.

## Style conventions

- Prefer **Tick** and **Tock** when naming the production contracts in prose.
  Preserve lowercase spelling in exact code identifiers such as `tick()`,
  `tick_block()`, and `tock_coverage()`, and in established shorthand where the
  surrounding document intentionally mirrors source/config notation.
- Use **random access** as a noun and **random-access** as an adjective.
- Use **persisted** for the authored output-retention contract and **persistent**
  for unrelated storage/process lifetime.
- Use **page version**, not "pages version."
- Prefer **global position/range** when discussing coordinate semantics; use
  **sample index** when the integer sample coordinate itself matters.
- Do not infer thread placement from a port contract. State **audio thread** or
  **background** when scheduling is relevant.
