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
Sequential input when its data has been prepared before the consuming pass.

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
or prepared use that requires its value.

Ephemeral does not mean stack-allocated, contiguous, unpaged, or recomputed for
every consumer. Background evaluation may create a transaction-local materialization
that several consumers share.

### Persisted output

A **persisted** output has the runtime retention guarantee defined by
`OutputRetention::persisted`: generated/finalized values remain retained while they
remain in output coverage, subject to version replacement and reader lifetime.

**Persisted does not mean serialized to disk.** Physical backing may be memory,
file-backed storage, mmap, or another representation that preserves the retention
contract.

Use **persistent** separately for ordinary physical lifetime or placement, such as
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
when scheduling order is the property that matters.

### Background evaluation DAG

The **background evaluation DAG** contains the dependencies required to satisfy
coverage demand outside the ordinary same-Tick schedule. It can contain authored
Tock execution, synthesized Tick replay, and dependencies traversed while proving or
executing replay. Valid persisted outputs may terminate traversal as stored
boundaries.

The background evaluation DAG is not the semantic dependency graph and is not a
second audio-thread scheduler.

### Background evaluation component

A **background evaluation component** is a statically planned component of the
background evaluation DAG with retained forward/reverse/evaluation ordering and
endpoint metadata.

## Scheduling contexts

### Audio thread / audio-thread execution

Use **audio thread** only when the scheduling or hard execution constraint actually
matters: no blocking, no request-sized allocation, pre-provisioned recording capture,
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
free capture-block capacity for recording. It is independent of background
evaluation progress.

## Coverage and change propagation

### Global position / range

Use **global position** or **global range** for the coordinate system in which
sample/event data and coverage are addressed. Use **sample index** when an actual
integer sample coordinate is specifically meant.

### Coverage

**Coverage** is the exact set of global positions for which an output semantically
exists and may legally be requested. `IndexedCoverage` is the current C++ type name
for this concept.

Coverage is independent of page boundaries and physical residency.

### Requested coverage

**Requested coverage** is the covered output region that a consumer or internal
completion operation requires to be available or computed.

### Changed region

A **changed region** is an exact region whose semantic value or coverage may have
changed. Forward propagation preserves exact changed regions; physical page
invalidation must not widen downstream semantic change merely because a whole page
becomes locally invalid.

### Invalidation

**Invalidation** marks previously valid data as not current for a target semantic
version. It does not imply immediate destruction of the currently published readable
representation.

Use **semantic invalidation** when distinguishing semantic staleness from physical
layout or allocation changes.

### Forward coverage propagation

**Forward coverage propagation** maps current input coverage, exact changed input
regions, and semantic/configuration change causes to output coverage and exact
changed output regions.

For authored Tock nodes this is provided by `propagate_forward_coverage()`; eligible
Tick replay uses compiler-synthesized propagation where the mapping is statically
known.

### Reverse coverage propagation

**Reverse coverage propagation** maps requested output coverage to required input
coverage. It is value-blind under the current design.

For authored Tock nodes this is provided by `propagate_reverse_coverage()`; eligible
Tick replay uses compiler-synthesized propagation.

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
uses it. Superseded physical versions may be reclaimed after their pins disappear.

## Physical representations and lifetimes

### `NodeStorage`

`NodeStorage` is the canonical fixed-layout storage owned for an executable
generation. It contains fixed node state and compiler-selected persistent regions.
It is not the owner of dynamically sized persisted-output pages or recording
capture backlogs.

### Persisted page

A **persisted page** is a retained/versioned physical unit used when a persisted
output has page-backed storage. Page validity is version-specific; an older readable
persisted page may remain pinned while a successor is invalid or being rebuilt.

Not every persisted representation must use this physical form: finalized
Tick/persisted data may use a different retained representation. Use **persisted
page** only when page-backed persisted storage is actually meant. Do not call an
ephemeral intermediate a persisted page.

### Transaction-local materialization

A **transaction-local materialization** is an ephemeral concrete representation
created for one background evaluation transaction and reusable by consumers within
that transaction.

For a Tock/ephemeral output feeding a Random Access consumer, the current design
uses a **transaction-local page-backed materialization** so the consumer receives
the required addressable representation. Replayable Tick work may avoid or fuse
materialization where the consumer contract permits it. A transaction-local
page-backed materialization is not a persisted page and carries no retention
guarantee beyond the transaction/readers that require it.

### Prepared sequential data

**Prepared sequential data** is background-produced data made available before a
future Sequential consumer runs. It is a delivery/materialization role, not an
output-retention mode; the source may be ephemeral or persisted.

### Capture block / capture log

A **capture block** is pre-provisioned storage used to copy a recording output at its
production point without request-sized audio-thread allocation.

The **capture log** is the append-only sequence of published capture records awaiting
background consumption. Capture blocks are transaction input, not persisted output
pages. Successfully committed transactions can return consumed capture blocks to
the allocator/reuse pool after their retained result has been materialized elsewhere.

## Source availability

### Live source

Use **live source** only when the relevant property is that historical values are
not inherently reproducible from retained data. An unreproducible ephemeral Tick
source needs an explicit recording policy before it can satisfy historical
random-access demand.

Do not use **live** as a synonym for Tick, Sequential, or audio-thread execution.

## Legacy source identifiers

Some current C++ identifiers retain older names, including `IndexedCoverage`,
`IndexedState`, `IndexedPlan`, `IndexedEndpointOrdinal`, and related structures.
Historical documents and older revisions also contain `Realtime*`/`Indexed*` port
configuration names.

When referring to an exact source identifier, preserve its spelling. Do not extend
that spelling into architectural prose. For example:

- `IndexedCoverage` is the source type; the concept is **coverage**.
- `IndexedState` is the source type; the concept is **Tock-only non-semantic
  acceleration state**.
- `IndexedPlan` is the source type; its contents describe semantic dependencies,
  coverage planning, background evaluation, persisted boundaries, and connection
  delivery facts rather than one architectural "indexed domain."

## Preferred replacements for ambiguous umbrella language

Do not mechanically replace an old umbrella word with one new umbrella word. Choose
by meaning:

| Avoid as an architectural category | Use when that is the actual meaning |
| --- | --- |
| realtime graph | same-Tick scheduling graph; semantic dependency graph; or audio-thread execution |
| realtime dependency | same-Tick dependency |
| realtime storage | sequential-consumption storage; persistent temporal state; or audio-thread-safe storage, as applicable |
| realtime path | audio-thread path, when the thread constraint is the point |
| indexed graph | background evaluation DAG or semantic dependency graph |
| indexed input | random-access input |
| indexed output | Tock output, persisted output, background-requestable output, or another narrower description |
| indexed access | random access |
| indexed execution | background evaluation |
| indexed transaction / indexed batch | background evaluation transaction |
| indexed semantic version | semantic version |
| indexed page/version | persisted page / page version |
| indexed result | random-access result, only when the external access mode is the point |
| temporary page | transaction-local materialization; transaction-local page-backed materialization when page addressing is specifically required |

If none of the right-hand descriptions is relevant, remove the old qualifier rather
than inventing a replacement.

## Style conventions

- Prefer **Tick** and **Tock** when naming the production contracts in prose.
  Preserve lowercase spelling in exact code identifiers such as `tick()`,
  `tick_block()`, and `tock_coverage()`, and in established shorthand where the
  surrounding document intentionally mirrors source/config notation.
- Use **random access** as a noun and **random-access** as an adjective.
- Use **persisted** for the authored output-retention contract and **persistent**
  for unrelated physical/process lifetime.
- Use **page version**, not "pages version."
- Prefer **global position/range** when discussing coordinate semantics; use
  **sample index** when the integer sample coordinate itself matters.
- Do not infer thread placement from a port contract. State **audio thread** or
  **background** when scheduling is relevant.
