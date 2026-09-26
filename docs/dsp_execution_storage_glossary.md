# DSP Execution And Storage Glossary

_Status: normative terminology for current DSP, GraphJit, coverage, storage, and executor design documents._

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
block execution. Its authored contract may include finite history.

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
matters: no blocking, no request-sized allocation, pre-provisioned Tick capture,
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

### Capture allocator

The **capture allocator** is the non-audio-thread provisioning role that maintains
the free-block reserve for the shared Tick-capture pool. Explicit recording and
Tick/persisted staging may consume blocks from the same pool. Provisioning is
independent of background-evaluation progress; a slow background worker increases
the sealed/pending backlog rather than changing the audio-thread allocation rules.

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

A **changed region** is an exact region whose semantic value or coverage may have
changed. Forward propagation preserves exact changed regions; stored page
invalidation must not widen downstream semantic change merely because a whole page
becomes locally invalid.

### Invalidation

**Invalidation** marks previously valid data as not current for a target semantic
version. It does not imply immediate destruction of the currently published readable
representation.

Use **semantic invalidation** when distinguishing semantic staleness from storage
layout or allocation changes.

### Forward coverage propagation

**Forward coverage propagation** maps current input coverage, exact changed input
regions, and semantic/configuration change causes to output coverage and exact
changed output regions.

For authored Tock nodes this is provided by `propagate_forward_coverage()`; eligible
Tick replay uses compiler-generated propagation where the mapping is statically
known.

### Reverse coverage propagation

**Reverse coverage propagation** maps requested output coverage to required input
coverage. It is value-blind under the current design.

For authored Tock nodes this is provided by `propagate_reverse_coverage()`; eligible
Tick replay uses compiler-generated propagation.

### Replay

**Replay** is background recomputation of an eligible Tick implementation using its
already generated/imported `tick_block()` wrapper. Intrinsic replayability is a node
trait; contextual replayability additionally depends on the whole upstream graph and
available data.

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

## Storage representations and lifetimes

### `NodeStorage`

`NodeStorage` is the canonical fixed-layout storage owned for an executable
realization. A logical graph revision may briefly have transition and steady
realizations, each with its own canonical `NodeStorage`. It contains fixed node state
and compiler-selected persistent regions.
It is not the owner of dynamically sized persisted-output pages or recording
capture backlogs.

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
the addressable representation must instead be materialized and published before the
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

### Current Tick representation

A **current Tick representation** is the mutable/current-block representation written
by Tick production and read by same-Tick Sequential consumers after the required
same-Tick dependency has executed. It is not a published persisted snapshot and is
never the baseline backing for a Random Access input.

### Capture block / capture log

A **capture block** is pre-provisioned storage that lets Tick-produced data escape the
ordinary current-block lifetime without request-sized audio-thread allocation. The
same allocator-managed block mechanism serves explicit recording and Tick/persisted
staging. When layout permits and the captured region is already final under the Tick
history/latency contract, the capture block may simultaneously be the producer's
current Tick data: Tick writes it once, same-Tick Sequential consumers read it
after the producer executes, and background persistence later consumes/adopts it.
Otherwise the generated path performs a bounded copy into a capture block when the
region becomes final.

A sealed capture block is immutable. A block that was acquired, written, or exposed
during one root `tick_block()` callback remains stable until that callback
boundary; it may be marked reclaimable earlier by another thread, but it is not
returned to the audio-thread free-block pool during the callback.

The **capture log** is the append-only sequence of sealed capture records awaiting
background consumption/publication. In the preliminary implementation, Random Access
reads do **not** consult this log: persisted data becomes Random-Access-visible only
through a published persisted-page version. After commit, copied capture blocks may
become reclaimable once callback/background ownership is gone; a data adopted by
the page store instead transfers ownership to the published page version.

### Published-snapshot Random Access

The preliminary Random Access implementation reads one immutable selected/pinned
published representation. For persisted outputs that representation is the
persisted-page snapshot. Pending candidates, current Tick buffers, and sealed but
unpublished Tick capture blocks do not extend Random Access coverage or visibility.
A Tick callback therefore cannot observe a newly produced Tick/persisted block until
a successor page version containing it has been published and a later callback/read
context selects that version.

A future optimization may overlay a bounded recent set of sealed Tick capture blocks
on top of the published persisted-page snapshot so a later node in the **same Tick**
can Random-Access newly finalized recorded data. That optimization requires a
same-Tick producer-to-consumer dependency, a source-selection branch (published page
versus recent capture block; ordered merge for events), and callback-boundary-safe
reclamation. It is deliberately not part of the preliminary implementation.

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
history; a Tick output owns its authored history and latency/future window. This is
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
original rebuild when both are necessary; `GraphExecutor` activates the transition
form at the splice and later reconciles currently evolved state into the already-
compiled steady form at a safe root callback boundary. Expiry is defined by absolute
semantic positions/ranges, even if a fixed-block implementation also precomputes the
equivalent callback count. A newer graph revision that arrives before expiry starts
from the active transition realization, not from its pending steady successor.

## Source availability

### Live source

Use **live source** only when the relevant property is that historical values are
not inherently reproducible from retained data. An unreproducible ephemeral Tick
source needs authored persistence or an explicit recording policy before it can
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
