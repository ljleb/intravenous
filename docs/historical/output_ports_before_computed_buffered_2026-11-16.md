# Output Port Contracts Before Computed/Buffered (2026-11-16)

_Status: non-normative historical excerpts. Superseded by
[Output Capabilities, Buffer Access, And Background Demand](../output_capabilities_and_background_demand.md)._

Provenance: original semantic text (with only relative Markdown links rebased) from the uploaded `intravenous(20261116-004408).zip`
repository snapshot, before the 2026-11-16 design-document revision. The sections
below retain former intent statements about Tick-only sequential outputs,
`OutputRetention::{ephemeral,persisted}`, strict retention, whole-Region
materialization, full-coverage publication, and replay-related coverage proof.
Line ranges refer to the archived original snapshot, not the edited documents.

## `dsp_execution_storage_glossary.md` original lines 1–129

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
| input access | **Sequential**, **Region**, **Coverage** | how an input consumes values |
| output production | **Tick**, **Region**, **Coverage** | how an output's values are produced |
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

### Region input / finite-region access

A **Region** input consumes one finite ordered region. The region's internal ordering
is intrinsic; it does not expose a configurable Coverage-ordering constraint.

### Coverage input / coverage access

A **Coverage** input may address arbitrary global positions inside its exact available
coverage. Its configured realization may constrain the ordering of the Regions that
form that coverage.

Use **random-access** as an adjective and **random access** as a noun phrase.
Random access is the umbrella for Region and Coverage access; it does not imply
background production or persisted retention.

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

### Region output / Region production

A **Region** output publishes one finite ordered region through background evaluation.
Its ordering is intrinsic rather than configurable.

### Coverage output / Coverage production

A **Coverage** output publishes arbitrary covered Regions through demand-driven
background evaluation. It may expose a `CoverageOrdering` guarantee.

### Tock callback

A **Tock** callback is the background callback that realizes Region or Coverage
production. It is not itself an authored output-production alternative.

Background production is not a synonym for random access or persistence. A Region or
Coverage output may be ephemeral or persisted and may feed any compatible input form.

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
[Batched Node Callback Direction](../batched_node_callbacks_direction.md).

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

---

## `coverage_and_background_evaluation.md` original lines 69–166

are permitted alongside synchronous callbacks; suspendable `_batch()` callbacks
are not required initially. Ready-frontier batching does not impose a single
planning visit per node.

The central rules are:

> Input access, output production, and output retention are independent authored
> contracts. `SequentialInputConfig` declares a bounded sequential read window;
> `RegionInputConfig` declares one finite ordered region; and
> `CoverageInputConfig` declares arbitrary reads within the exact bound coverage
> (explicitly selected full Regions during Tock evaluation).
> `TickOutputConfig` selects sequential production by the existing generated
> `tick_block()` implementation; `RegionOutputConfig` and `CoverageOutputConfig`
> select demand-driven production by the appropriate Tock callback. Any output may be
> `ephemeral` or `persisted`.

> One concrete executable primitive nevertheless belongs to exactly one execution
> domain. It authors one Tick form or one Tock form, never both, and all of its outputs
> use that family's production forms. A module node groups separate primitives when an
> authored abstraction needs both domains. Eligible Tick replay is generated
> off-thread Tick execution; it is not an implicit Tock callback. Authored Tock never
> synthesizes an implicit Tick callback.

> All concrete GraphJit node types have static constexpr port schemas. Graph
> topology, node instance count, and connections remain dynamic graph data.
> Replayability is a separately declared and validated **node trait**, not a new
> `tick()` contract, port field, or alternate DSP callback. Contextual replayability
> additionally requires every upstream value for the requested coverage to be
> available or reproducible.

> `tock_coverage()` and its propagation/evaluation transactions run **only off the
> audio thread**. An audio-thread sequential input reads the currently published
> persisted-data root even when it is out of date; for a missing position it supplies that particular
> input's `neutral_value`. No cache miss invokes tock or blocks the audio thread.

> A random-access input may consume a Tock output (through canonical persisted data or an
> ephemeral addressable materialization), a Tick/persisted output's published data,
> or a contextually replayable Tick output. An unreproducible Tick/ephemeral output
> cannot directly satisfy random-access demand: the DSP author must select persistence
> or place an explicit recording node at the authored boundary. The restriction
> prevents **implicit recording**, not a technically impossible connection. The
> preliminary Tick-time Random Access path reads only a pinned published/materialized
> immutable view; it never aliases current mutable Tick output.

> Tiling is a non-materializing composition of source channels. It preserves each
> source's production, retention, and effective access capabilities. A whole-tile
> random-access connection requires every selected source channel to satisfy the
> requested random-access contract; channel projection retains its own capabilities.

> Computed Tock continuous and event outputs publish an exact finite logical
> Region-range list. A Region output publishes exactly one range; a Coverage output
> publishes zero or more ordered ranges. The runtime derives the exact positional
> `IndexCoverage` union. Forward propagation
> may conservatively over-report positions whose values could have changed; reverse
> propagation may likewise conservatively over-request input positions. Under-reporting
> either dependency direction is a correctness error, while over-reporting is a
> performance cost. Physical record boundaries never widen the logical Region list or
> the already-reported forward may-change positions. Mechanically pointwise replayable
> Tick dependencies may still use compiler-generated propagation.

> Event Region lists are anonymous in the initial contract. A callback can use list
> position to keep Regions separate during that evaluation, but no stable Region ID is
> promised across evaluations. A Sequential event consumer receives a merged
> timeline-ordered stream in which overlapping Regions and equal-time duplicates are
> preserved.

> Invalidation and requested coverage are processed as batched background
> evaluation transactions. All roots available for a selected transaction are
> accumulated and compatible requirements are unioned. Forward/reverse planning
> may revisit a node after explicitly awaited information becomes ready; ready
> work is batched where possible. Ordinary Tock value production remains separate
> from suspendable propagation. Tock `State` remains non-semantic acceleration
> available only to tock.

> No unresolved random-access computation dependency may participate in a cycle.
> Replayable tick subgraphs must be acyclic when expanded for background demand;
> valid published persisted values can terminate replay traversal. Ordinary
> Tick-to-Sequential feedback retains its existing scheduling semantics.

> Persisted output data are **never evicted** for age, cache limits, memory
> pressure, or invalidation. Retained regions/chunks may be removed only when
> output coverage no longer includes them or an authored recording publishes an
> explicit erasure; replacement storage versions are
> reclaimed only after their readers release them. Unbounded memory use is an
> explicit consequence of the author's persistence declaration.

> A Tick/persisted output is the recording output; there is no third retention mode or
> separate recorder-port marker. An explicit recording node is an ordinary authored
> node that consumes the live Sequential input and writes a Tick/persisted output. It
> uses already-provisioned producer-reserve storage for each block actually written to
> that output. Leaving the output untouched preserves
> previously recorded RAM data at that timeline position; `write_void()` publishes an
> authoritative erasure. Each background pass independently pins one finite prefix
> from every relevant producer queue; selected changes enter ordinary exact
> invalidation, reverse planning, background computation, and coherent publication.
> Pending queue storage is transaction input, not the recorder's Random Access
> representation.


---

## `coverage_and_background_evaluation.md` original lines 170–201

## 1. Port model

Port data/value type, input access, output production, and output retention
are independent. The callback that *uses* an input does not determine that input's
access: a `tick_block()` implementation can consume random-access inputs, including
when that Tick implementation is replayed in the background.

The independent output-production axis applies across schemas and module composition,
but one concrete executable primitive is homogeneous: exactly one Tick callback form
owns only Tick outputs, or exactly one Tock callback form owns only Region/Coverage
outputs. This keeps realtime scheduling and background evaluation as disjoint
primitive domains. Grouping both domains is the job of a module node.

| Axis | Authored alternatives | Meaning |
| --- | --- | --- |
| input access | `SequentialInputConfig`, `RegionInputConfig`, `CoverageInputConfig` | bounded sequential window, one finite ordered region, or arbitrary positions inside coverage |
| output production | `TickOutputConfig`, `RegionOutputConfig`, `CoverageOutputConfig` | generated Tick data, one finite background region, or background coverage |
| output retention | `OutputRetention::{ephemeral,persisted}` | whether generated finalized values must be retained |
| data | registered continuous value type, event type | value/event representation and bounds |

Sequential input history and Tick output history/latency are finite realization
constraints. Region/Coverage input and output declarations do not acquire these
properties. Output retention never selects a callback. The concrete sample/event
schema is in section 13; the direct-connection rule is in section 22.

`tick()` already exists: the node traits generate its `tick_block()` wrapper, and
GraphJit imports that wrapper's LLVM definition. The new replayability trait
permits **some** such nodes to execute through that same wrapper in the background
evaluation DAG. It does not make all `tick()` implementations pure or replayable.
No `tock_coverage()` call occurs on the audio thread.

## 2. `IndexCoverage` is the positional coverage boundary

---

## `coverage_and_background_evaluation.md` original lines 365–405

## 5. Sparse logical requests and stored materialization

External random-access requests may be sparse. UI waveform rendering may ask for a few
positions from a very large coverage, for example. Sparse demand behaves according to the output's access and retention semantics, not according to a generic cache policy.

A `tock/ephemeral` output has no persisted materialization obligation. A sparse
request is intersected with exact coverage; the runtime selects the complete
logical Regions implicated by that request. Its `tock_coverage()` work then
computes the selected Regions, writing directly into caller/result or
transaction-local storage. In that
callback, each random-access input's visible Region/Coverage is restricted to
the exact logical input Regions selected by reverse propagation, even if
other source data have already been computed. A selected logical Region is
kept whole, not replaced by a clipped pseudo-Region.

A published `tock/persisted` output is different: the whole output coverage has
already been materialized before publication. A sparse external request merely
reads the requested subset from that complete stored representation. It does not
cause the executor to create a partially populated stored output.

A persisted candidate applies exact-range overwrites and erasures. If a mutation
replaces `[20,21)` inside a retained region covering `[10,100)`, the store preserves
the unaffected `[10,20)` and `[21,100)` values and inserts the replacement record for
`[20,21)`. It does not invalidate or recompute unrelated positions merely because
they once shared an allocation unit. Once the background transaction has completed
all semantic demand required for its target output coverage, the candidate may be
published as one `(semantic_version, data_version)` result.

A tick/persisted output retains finalized sequentially produced values; its
published coverage is a random-access **stored boundary** without requiring tock
or an additional recording node. A replayable tick/ephemeral output can instead
satisfy random-access demand by recomputing from available upstream values. Only
an unreproducible tick/ephemeral source requires explicit recording before such
demand. Semantic changes to any published retained source start downstream sound
invalidation; they do not erase the older readable published data.

There is no persisted sample page width or page index. Changing the whole-graph root
block size changes Tick scheduling but does not repartition canonical persisted
samples or events and requires no repaging transition.

### Vue output queries are consumers of existing coverage

---

## `coverage_and_background_evaluation.md` original lines 604–681

## 10. Tock coverage evaluation callbacks

`tock_coverage()` produces values/events into prepared output storage for
**only the selected exact demanded covered output regions**. Its multi-node
`tock_coverage_batch()` form remains an optimization over independent scalar
invocations: every lane has its own exact output request, input requirements,
bindings and Tock `State`.

**Explicit input selection invariant:** for every `RegionInputConfig`
and `CoverageInputConfig` bound to Tock, the readable `region()`/`regions()`
contains exactly the complete input Regions deliberately selected by the
reverse planner and Region closure, not extra cached Regions. The input's
`coverage()` convenience (where supported) is the positional union of those
selected Regions, **not** the possibly smaller reverse-required positional
set. Overlapping and duplicate Regions remain separate, and a selected event
Region may legitimately contain zero events. Accessing unselected Regions is
invalid despite their presence in retained storage. Planning-only discovery
reads do not enlarge this finalized input selection.

Thus, at fixed semantic/data versions and authored configuration,
cache history cannot alter selected callback-visible input Regions or computed
results. Compatible demand roots can be coalesced before invocation, so
different demands may select different Region collections; nevertheless any
overlapping output positions must have equivalent computed results, independent
of demand grouping. This also holds for each batch lane independently.
The ordinary evaluator does not need an input `required_coverage()` or
upstream-wide `produced_coverage()` accessor: requirements belong to reverse
planning; readable input `regions()` and selected writable output `regions()`
provide the semantic evaluation bounds. See
[section 6 of the automation/propagation direction](../automation_and_suspendable_coverage_direction.md#6-tock-input-and-output-views)
for examples and the distinction from the current source binding.

The callback can inspect the selected sample rate and the explicitly
provided input Region views, but cannot read mutable/unavailable live Tick data or depend on
Tick-owned `State`. Tock-owned `State` is solely non-semantic acceleration:
it cannot affect values, coverage, input requirements or output Region lists.
Successful sample completion initializes every demanded covered sample/channel;
event completion emits every event within each demanded covered Region, including
valid zero-event results. Events remain in nondecreasing timestamp order per
Region, preserving equal-time producer-local order. Cancellation and failure
publish no partial results.

Ordinary Tock callbacks remain separate from propagation coroutines. A node
cannot observe unrequested data through its Tock inputs simply because its
planner earlier requested larger discovery regions. `tock_coverage()` never
executes on the audio thread; replayable Tick callbacks remain Tick callbacks.

## 11. Coalesced demand and coherent candidate completion

A transaction collects external random-access fetches, future sequential
playback materialization demands, selected fixed producer-queue prefixes, and
internal stale/absent `tock/persisted` completion requirements. Coalesce ready
compatible requirements without changing their exact covered semantics. Pure
reads of compatible published persisted data may stop without invoking Tock.

When a forward or reverse planner needs unmaterialized data, it registers an
explicit prerequisite that the scheduler can satisfy through reverse
requirements and background value production. This can happen before forward
coverage is completely known. Dependency resolution, forward invalidation,
reverse planning and required materialization may interleave; the prior fixed
one-forward-pass/one-reverse-pass ordering is not normative. Separate output
ports, information kinds (coverage or contents) and required positions may
become internally ready independently, avoiding artificial whole-node cycles.

No unresolved true requirement cycle is permitted. Readiness distinguishes
known-empty coverage from coverage not yet discovered. The scheduler does not
spin on an unresolved callback with no concrete pending requirement, and
superseded work cannot publish. A candidate is committed only when every
required persisted result is complete for the selected semantic/data version;
unrelated retained records and subtrees remain shared as before. Realtime Tick
execution never blocks waiting for new background discovery.

The ordinary `tock_coverage()` and replay evaluation operations are invoked
with their selected prepared requests. The scheduler may preserve efficient
single-invocation/coalesced evaluation when dependencies allow it. A hard
single-visit rule does not constrain **planning**, and scratch/storage decisions
must not leak unrelated cached or exploratory input Regions into evaluation.


---

## `coverage_and_background_evaluation.md` original lines 707–826

## 13. Authored port schema, retention, and replayability

The port schema has independent input access, output production, and output
retention. Data properties remain separate for samples and events:

```cpp
SequentialInputConfig<Types...>
RegionInputConfig<Types...>
CoverageInputConfig<Types...>

TickOutputConfig<Types...>
RegionOutputConfig<Types...>
CoverageOutputConfig<Types...>

enum class OutputRetention { ephemeral, persisted };
```

These are abbreviated authored-schema sketches. Event declarations have the same
three access and production forms but remain separate event config types. Sequential
history and Tick output history/latency are contributed by each configured instance
through `constrain_ports()` rather than stored in static authored configs. Reflection
then erases the heterogeneous typed declarations into the configured graph's access
and production variants.
Input access, output production and output retention are independent in reflection,
configured-graph serialization and the compiler record interface. GraphJit classifies
per-channel compatibility and delivery using those orthogonal contracts; execution of
the retained background plan begins in step 4.

| production | retention | producer and retention semantics |
| --- | --- | --- |
| tick | ephemeral | sequential generation; no post-window retention obligation |
| tick | persisted | sequential generation; every written/voided callback block is retained |
| region or coverage | ephemeral | background demand-driven generation; transaction-local materialization permitted |
| region or coverage | persisted | background demand-driven generation; generated covered data retained |

`ephemeral` does not forbid transaction-local materialization. `persisted` is a strict runtime
retention guarantee, not a best-effort cache or a promise of project-file
serialization. No persisted data are evicted for memory pressure, age, inactivity,
invalidation, or lack of readers. Retained regions/chunks are removed only when output
coverage ceases to include them; old storage versions are freed only after their
pins are released. A persisted Tick output becomes subject to that guarantee whenever
one callback leaves the output `written` or `voided`; the addressed callback block is
then mandatory recording work. An untouched callback block makes no retention change.
An author opting into persistence also opts into potentially unbounded memory use;
the compiler does not invent an eviction or recording policy to cap it. Backing
storage (RAM, file, mmap, etc.) is an implementation choice that must preserve this
guarantee.

`TickOutputConfig` does not imply that its output can only be read sequentially.
Persisted Tick data can be read at recorded published positions; an ephemeral Tick
output may also be randomly accessible when GraphJit proves contextual replayability.
No retention or access mode is inferred from a tiled expression: each member keeps
its own contract.

### Replayable node trait

A node opts into the intrinsic replay **type trait** by declaring
`static constexpr TickReplayMode replay_mode` as `input_union` or `full`
(default `disabled`; owner decision, 2026-11-15). The mode also selects the
compiler-generated output coverage: the union of the input coverages, or the
whole circle; see
[Sequential inputs resolve absence](../automation_and_suspendable_coverage_direction.md#4-absence-and-sequential-fallback).
`iv::details::tick_replay_mode_v<Node>` is the validated compiler-facing value,
and `intrinsically_replayable_v<Node>` derives eligibility from it; neither is a
port field or an additional DSP callback. The earlier boolean declaration is
preserved [historically](replay_coverage_before_tick_replay_mode_2026-11-15.md). An opted-in node must define the
existing `tick()` but **not** its own native `tick_block()`, have no `State`, no
input/output history or latency after resolution, and zero internal latency. Random
Access inputs are permitted. Its tick computation must be deterministic and side-effect-free
under one immutable semantic/configuration version, declared inputs, and absolute
sample position; no mutable external or live-only resource may affect its result.
Validate instance-dependent properties such as internal latency after configuration.

A replayable Tick node must declare only ephemeral Tick outputs, for both
continuous values and events (owner clarification, 2026-11-15). It can consume
another node's persisted output as a recorded dependency. The output-declaration
restriction does not prohibit these incoming connections.

Event replay, if enabled, must additionally preserve deterministic timestamp/order
and declared capacity; do not silently infer event replay from the sample rule.

Existing node traits already generate the `tick_block()` wrapper for `tick()`;
GraphJit already imports its LLVM definition. Replay uses **that** imported
wrapper in the background evaluation DAG, allowing normal O3 inlining, fusion,
value specialization, and possible SIMD. It does not redefine `tick()` for ordinary
nodes or introduce a second implementation of the DSP algorithm. GraphJit generates
identity-time forward/reverse propagation for mechanically pointwise dependencies.
When a replayable Tick node has a non-pointwise Random Access dependency, such as a
source addressed by a `FractionalGlobalIndex` coordinate stream, its background dependency
mapping is supplied through the same forward/reverse propagation contracts used by
background planning. Conservative whole-input/whole-output dependency bounds remain legal where
sound; suspendable propagation may instead read selected input data to discover
tighter bounds or exact output existence when required.

The trait gives **intrinsic eligibility** only. An output is contextually
replayable for requested coverage only if all of its required upstream inputs are
reproducible or available in finalized published data for the selected semantic
version. A pure gain node fed by an unrecorded microphone is not historically
replayable; the same gain fed by recorded data can be. A retained published tick
output terminates traversal even if its original producer cannot replay. Replay
uses isolated invocation-local scratch, immutable configuration and a canonical
block-position/alignment contract; it cannot mutate shared live execution state.

Random Access replay dependencies are an availability requirement, not an alternate
execution mode inside `tick()`. Before a background replay invocation, the background
transaction resolves the selected immutable versions, runs any required Tock/replay
work, and installs the planned Region/Coverage views. During ordinary audio-thread
Tick execution the same rule is stricter: every Random Access input is already bound
to its callback-pinned persisted/materialized view before `tick_block()` begins. Tick
code never invokes Tock, performs dependency discovery, waits for materialization, or
extends its Random Access coverage in response to values it reads.

This replay mechanism is also the current answer for procedural values that can be
cheaply recomputed at arbitrary positions: they remain eligible constrained Tick
nodes rather than introducing a fourth Region/Coverage/Sequential access form. A
future explicit `tack()`/`tack_block()` callback family has been considered as a way
to make replayability first-class while returning `tick()` to foreground-only
semantics, but it is deliberately **not** current direction.

## 14. Canonical persisted data uses explicit sample regions and event chunks

---

## `coverage_and_background_evaluation.md` original lines 1396–1442

## 22. Direct connection compatibility and explicit recording

The port validator checks **each source channel** against the destination input's
access contract, not equality of the producer callback and consumer callback.
These are connection permissions, not guarantees of audio-thread scheduling:

| Source output | Sequential input | Random Access input |
| --- | --- | --- |
| Tick/ephemeral, unreproducible for demanded coverage | allowed | authored Tick persistence or an explicit recording node required |
| Tick/ephemeral, contextually replayable | allowed | allowed by background replay into an addressable materialization |
| Tick/persisted | allowed from the current Tick representation | Coverage input only, through the selected published persisted-data snapshot; a Region input is rejected |
| Tock/ephemeral | allowed with a materialized sequential/addressable window | allowed with transaction-local materialization in background or materialized addressable data for Tick use |
| Tock/persisted | allowed from published data generated ahead of playback | allowed through the selected published persisted-data snapshot |

An unreproducible Tick/ephemeral output cannot acquire implicit historical storage
merely because it feeds a random-access input. The restriction applies whether the
random-access input is consumed by tick or tock. A Tick/persisted recording has a
second restriction: it may feed a Coverage input but not a Region input. Recording
writes are positional overwrites. Their `IndexCoverage` merges overlapping and
touching ranges into maximal contiguous Regions, so individual Tick block boundaries
do not become authored Region identities. Tock outputs may feed sequential inputs.
Tiling does not change these permissions or silently materialize a new output: a
whole-tile random-access connection requires every selected source channel to satisfy
it. Projections preserve their source channel's original capability.

A Tock-produced output directly feeding another node's Random Access input must
be materialized into an immutable addressable representation before that consumer
evaluates. For an ephemeral output, a background-only consumer may use a
transaction-local addressable materialization; a Tick-time consumer requires a
materialized addressable window that survives through the callback. Persisted data
follow the strict retention guarantee. Replayable tick subgraphs
may be fused and materialized only where their consumers' addressing contracts
require it. Every tock callback, forward/reverse propagation callback, and persisted-data
recomputation runs off the audio thread.

An explicit recording node is required where an unreproducible ephemeral tick
stream must supply historical random-access demand. It declares an ordinary
Tick/persisted output; no additional recorder port kind or retention value exists.
Its semantics are fixed: output
writes overwrite the RAM recording at their timeline range, leaving an output
untouched preserves any previous recording there, and `write_void()` erases the
range authoritatively. Seeking changes the addressed timeline position, not those
rules. GraphJit does not insert an implicit generic recorder. Any independently
authored Tick/persisted producer is already recording its finalized output and needs
no additional recorder boundary.

### Storage requirements are inferred over overlapping port subsets

---

## `coverage_and_background_evaluation.md` original lines 1699–1725

## 23. Complete persisted outputs and backing storage

The persisted-data store is canonical for both Tick/persisted and Tock/persisted
outputs. A candidate successor is never exposed as a partially complete published
snapshot. Tock/persisted completion may require its entire exact candidate coverage
to be materialized before publication; Tick/persisted data become eligible when
their recorded callback blocks are incorporated into a coherent successor data
version.

Complete materialization does not require all bytes to remain in anonymous RAM.
Backing storage may use mmap/files, compression, deduplicated immutable regions/chunks,
or copy-on-write roots, but **never automatic eviction** of persisted covered
data. Invalidation retains readable old published data while a successor is
computed. Records may be removed from the logical retained output only when its
coverage no longer includes them. Superseded immutable storage versions may be
reclaimed after their readers release them. Any representation accessed directly by the audio-thread Tick path must meet
audio-thread access requirements.

Tick/persisted output differs only in **how persisted data are populated**:
`tick_block()` may revise storage under normal history/latency semantics, while each
recorded Tick invocation authors its addressed callback block as a whole or leaves it
untouched. Tock output retains exact arbitrary-coverage authoring and does not acquire
this block disposition. Once published, Tick- and Tock-produced persisted data use the
same range lookup, versioning, pinning, and consumer read path.

## 24. Realtime-produced persistence/recording queues and reclamation


---

## `coverage_and_background_evaluation.md` original lines 2502–2570

## 33. Summary invariants

1. Concrete GraphJit nodes have static constexpr port schemas; only graph topology
   and instance/connection composition are dynamic.
2. Input access (`SequentialInputConfig`/`RegionInputConfig`/`CoverageInputConfig`),
   output production (`TickOutputConfig`/`RegionOutputConfig`/
   `CoverageOutputConfig`) and output retention
   (`ephemeral`/`persisted`) are independent declarations.
   One concrete executable primitive still selects exactly one primary callback
   domain and declares only outputs produced by that domain; module nodes compose
   Tick and Tock primitives.
3. `tick()` already lowers to an imported, optimizable generated `tick_block()`.
   The planned batch layer retains `tick()` as a first-class authoring form and
   normalizes every accepted Tick shape to both `do_tick_block()` and
   `do_tick_block_batch()`. A separately declared node trait establishes intrinsic
   replay eligibility; GraphJit proves contextual replayability through available
   upstream coverage.
4. An unreproducible Tick/ephemeral source cannot directly satisfy Random Access
   demand. Tick/persisted, replayable Tick, and either Tock output can. Persisted
   outputs use canonical persisted sample regions/event chunks; ephemeral Tock/replay
   results use immutable
   addressable materialization with transaction or materialized-window lifetime according
   to the consumer callback context.
5. Tiling preserves per-channel contracts and creates neither an implicit recording
   policy nor an implicit producer.
6. `tock_coverage()`/`tock_coverage_batch()` and authored scalar/batched propagation
   callbacks run only off the audio thread. A sequential input plays stale published
   persisted data as-is and substitutes **its own** `neutral_value` for an uncovered
   requested position without waiting.
7. Persisted outputs retain all generated/finalized covered data without automatic
   eviction. Coverage removal alone ends the semantic retention obligation;
   superseded storage versions are reclaimed only after readers release them.
   Memory growth is author-selected.
8. Exact output `IndexCoverage`, conservative forward may-change ranges and
   conservative reverse requirements are independent of persisted record boundaries. Both dependency
   directions may over-approximate but never under-approximate actual dependencies.
   Mechanically pointwise replayable Tick propagation is generated from static
   dependencies; non-pointwise Random Access footprints use authored background
   propagation.
9. The background evaluation DAG has no unresolved cycles; feedback in the
   same-Tick scheduling graph retains its separate temporal semantics.
10. Realtime-produced recording and Tick/persisted data use producer-specific reserves and background pending queues backed by power-of-two blocks. Producers build chains
    privately and publish them cheaply at pass boundaries; background independently
    pins one finite prefix per queue. There is no cross-queue atomic snapshot. Queue
    insertion is not persisted-data publication, and released blocks return
    through non-realtime capacity management.
11. Tock-owned `State` is non-semantic Tock-only acceleration; canonical persisted-data
    stores and pending producer queues are background-owned sidecars, distinct from
    realtime fixed `NodeStorage`.
12. Storage requirements are joined over exact overlapping port atoms, not chosen
    independently per connection or promoted wholesale per authored port. storage
    coalescing happens only after those correctness requirements are known.
13. The legacy generated-node graph executor and dynamic concrete-port fallbacks
    are deleted; the package/configuration JIT and GraphJit's imported concrete-
    node LLVM remain.
14. F/R propagation produces prepared coverage only. Coverage/data publication and
    release of the selected producer-queue prefixes become authoritative together at
    final transaction commit; every failure path leaves committed state and required
    pending inputs unchanged.
15. The generated background root owns static node/materialization interleaving and
    calls only narrow transaction-local prepare/finalize hooks. Generated Tick and
    background roots receive explicit binding frames, never executor pointers, a
    persisted-data store, queue/capacity-manager internals, or a transaction object.
16. Tick reader pins and `TickMaterializationSnapshot` lifetimes cross one complete
    root callback without audio-thread allocation, blocking or final-owner
    reclamation. Replay work is split into compiler-legal block-sized invocations.


## Sparse Sample events and replayed curve readers (2026-11-13)

---

## `random_access_port_data_direction.md` original lines 36–149

## 1. Random-access sample data is Region or Coverage

Random-access sample data should no longer be exposed as a page store, a page
iterator, or a single-sample random accessor.

The authored/runtime-facing sample representation is instead one of two forms:

- **Region**: one logical range backed by one contiguous sample buffer;
- **Coverage**: a collection of zero or more Regions, where every Region independently
  owns/references one contiguous sample buffer.

A Region conceptually carries at least:

```text
logical begin/range
pointer to contiguous sample storage
frame/sample count
channel/layout information needed to interpret that storage
```

The exact C++ view type remains an implementation/API detail, but the contiguity
contract does not. A Region must be directly consumable by libraries such as FFT
implementations that require one contiguous input buffer.

Current mono/stereo and planar/interleaved layouts remain meaningful. Random-access
materialization should preserve a producer's useful/native channel layout where
possible rather than requiring an avoidable planar/interleaved conversion merely to
cross the port API. Future channel counts must fit the same model.

### Node callbacks receive contiguous views

Within both `tick*()` and `tock*()` contexts:

- a Region random-access sample input exposes one contiguous Region view;
- a Coverage random-access sample input exposes the Regions in its Coverage, with a
  pointer and size/range for each contiguous Region;
- node code should not need to walk storage pages;
- node code should not be forced through a single-sample random-access accessor when
  the underlying data is already contiguous.

The runtime/compiler may still use whatever ownership/indexing structures are useful
internally, but those structures are below the authored random-access contract.

### Coverage outputs receive pre-allocated Regions

A Coverage continuous output exposes a fixed, pre-allocated list of writable Region
views to its `tock_coverage()` callback. The output's forward propagation callback
publishes the complete ordered list of logical Region ranges, including overlapping
and duplicate ranges. The runtime derives the positional `IndexCoverage` union used
for change and demand propagation from that list. The union does not replace the list.

After forward and reverse propagation have completed, the runtime selects the Regions
required by the transaction and allocates their descriptors and contiguous backing.
This happens before generated background evaluation calls `tock_coverage()`. The
callback fills the supplied buffers. It cannot append a Region, change a Region range,
or allocate output backing. A Region continuous output uses the same representation
with exactly one Region.

Event Region and Coverage outputs use the same logical shape. Forward propagation
publishes one range for a Region output and an ordered range list for a Coverage
output. `tock_coverage()` receives one `WritableEventRegion` or an iterable
`WritableEventCoverage`. Each event Region has independent event storage even when
two ranges overlap or are identical. A Coverage input receives the matching iterable
`EventCoverage`; a Region input receives exactly one `EventRegion`.

For example, a node can preserve two anonymous event groups without encoding a group
identifier into the event value:

```cpp
void propagate_forward_coverage(
    PropagateForwardCoverageContext<MyNode>& ctx) const
{
    std::array const ranges{clip_a_range, clip_b_range};
    ctx.output<"events">().publish(ranges);
}

void tock_coverage(TockCoverageContext<MyNode>& ctx) const
{
    auto output = ctx.output<"events">().regions();
    output[0].write(first_clip_event);
    output[1].write(second_clip_event);
}
```

The two Regions may use the same `GlobalIndexRange`. Their event sequences still do
not alias or merge in the Coverage callback. A Sequential consumer of that output
receives one timeline-ordered stream. Events from all Regions remain present;
equal-time events from one authored Coverage output retain Region order. Event fan-in
retains the existing semantic source order before Region order.

Each Region has an independent writable storage range. Two Regions whose logical
ranges overlap must not alias the same value storage for their overlapping positions;
the two values are separate contributions. The runtime may place several Regions in
disjoint byte ranges of one larger allocation, but this physical allocation choice
does not make their value storage shared.

If any demand intersects a logical Region, the runtime retains that Region's complete
range. It must not expose a clipped intersection as a new logical Region. This keeps
the node-facing Region list independent of transaction request boundaries. If that
complete range makes the positional replacement domain intersect another Region, the
producer receives that second complete Region too. This continues until the
replacement domain stops growing. A consuming callback still receives only the complete Regions selected
by its own resolved input requirement; extra producer Regions cannot become
visible merely because other work materialized them. For
`tock_coverage()` and each `tock_coverage_batch()` lane, the bound
`region()`/`regions()` contains the explicitly provided, readable Regions;
`coverage()` is the union of those Regions, not the narrower reverse-required
positional set. The final selection is independent of retained/cached data
outside the requirements. A node whose Region ranges
depend on input data must determine and retain those ranges during suspendable
forward propagation, before value production. See
[Cyclic Automation And Suspendable Coverage Propagation](../automation_and_suspendable_coverage_direction.md).

## 2. Coverage regions may overlap

---

## `random_access_port_data_direction.md` original lines 322–351

## 5. Region/Coverage compatibility is asymmetric

Ignoring independent lifetime/recording requirements for the moment, the sample
shape/access compatibility is:

| Producer representation | Region input | Coverage input | Sequential input |
| --- | --- | --- | --- |
| Region | yes | yes, as one Region | yes |
| Coverage | **no** | yes | yes, by bounded reduction |
| Replayable Tick/ephemeral stream | effectively Region-shaped for one replay invocation | yes when lifetime/access requirements are satisfied | yes |
| Tick/persisted recording | **no** | yes, as canonical `IndexCoverage` Regions | yes |

A Region producer provides a stronger contiguity guarantee than a Coverage producer.
A Coverage producer cannot directly satisfy an input that requires one contiguous
Region. Doing so requires an explicit materialization/copy policy rather than an
implicit hidden copy.

A Tick/persisted recording is also Coverage-shaped. Its Tick writes are positional
overwrites, not authored Regions. `IndexCoverage` merges overlapping or touching
recorded ranges into maximal contiguous Regions and discards the original Tick block
boundaries. The recording can therefore contain zero or more Regions and cannot
satisfy a Region input, even when its current coverage happens to be contiguous.

This compatibility is only one axis of connection validity. Existing temporal and
lifetime constraints remain independent. In particular, a Sequential/Tick value may
still require an explicit recording/persistence policy before it can satisfy arbitrary
random-access lifetime requirements. The Region/Coverage distinction adds a
contiguity requirement; it does not erase the existing access/lifetime requirement.

## 6. Continuous port values use a closed registered type system

---

## `random_access_port_data_direction.md` original lines 965–998

## 9. Persisted and ephemeral random-access sample data stop using pages

The target architecture should remove page-based sample storage for Tock-generated
and other random-access sample data.

Persisted versus ephemeral continues to specify lifetime/retention, but neither
contract implies fixed-width pages. The retained/materialized values are Region or
Coverage data with contiguous storage per Region.

Consequences include:

- node-facing APIs no longer expose or depend on page width/page indices;
- persisted storage preserves Region/Coverage data and its contiguous backing rather
  than canonical fixed-width sample pages;
- ephemeral Tock results use the same Region/Coverage shape contract with shorter
  lifetime/ownership;
- sparse logical gaps consume no neutral-filled backing merely because distant
  Regions share one output coordinate space;
- FFT/convolution and similar consumers can require Region input and pass its
  contiguous storage directly to libraries that require it.

The executor-owned `PersistedDataStore` provides the retained ownership, version, and
publication boundary. It publishes one coherent immutable root. Authored Tock
continuous outputs use independent contiguous `PersistedSampleRegion` records.
Tick/persisted continuous recording uses interleaved sparse copy-on-write pages and
maps each canonical `IndexCoverage` range into one virtually contiguous
`PersistedSampleRegion`. A path-copied physical-run index makes virtual mapping
proportional to the number of changed/coalesced backing runs rather than the number of
retained pages. Event outputs use explicit-range `PersistedEventChunk` records.
`PersistedDataStore::Snapshot` binds these records to
`ContinuousRegionStorageView` at the runtime boundary.

## 10. Region count is evaluation data, not realtime layout data


---

## `automation_and_suspendable_coverage_direction.md` original lines 473–493

### Full-coverage proof before feedback validation

Accepted clarification, 2026-11-15: a Sequential/Tick SCC is not rejected just
because it contains `input_union` nodes. The compiler first proves which live
Tick outputs are fully covered and propagates that proof through generated
union rules. One fully covered contribution reaching an input proves a
union node's outputs full. The proof follows the actual port routes and
channel composition. It does not infer an authored forward callback's result.

Fully covered producers may be inside or outside the SCC. Even a component
made entirely of `input_union` nodes can therefore have full coverage when an
independent fully covered contribution reaches it. Reject only the recursive
sparse-coverage dependencies that remain unresolved. Do not introduce delayed
coverage history or a background fixed-point scheduler.

This proof concerns live coverage. It does not make cyclic ephemeral value
dependencies replayable; contextual background replay retains its separate
acyclic-dependency requirement and selected persisted boundaries.

### Playback preparation and missed background deadlines


---

## `automation_and_background_migration_plan.md` original lines 1–60

# Automation And Background Planning Migration

_Implementation coordination, 2026-11-15. This is an implementation order, not
another normative contract. Current source progress is recorded below._

The governing decision is commit `d2e1d0ec`, especially
[Cyclic Automation, Event Projections, And Suspendable Coverage Propagation](../automation_and_suspendable_coverage_direction.md).
The implementation baseline is `6826b784` (`automation clip v1`). The worktree
was clean when this comparison began. The earlier Stage 3 checkpoints describe
that baseline, not completion of this migration.

## What changes

- A point describes an outgoing `empty`, `hold`, or automatic `bezier` segment.
  Zero points produce empty coverage. `empty` means no coverage, not zero-valued
  samples. The last point connects to the first in circular index order.
- `CurvePointEvent` retains value, outgoing policy, and tension. `SampleEvent`
  does not contain enough information. Point IDs remain editing identities.
- Exact output coverage can depend on input contents. Forward and reverse
  callbacks can therefore suspend while the runtime materializes explicitly
  requested discovery inputs. They retain their different semantic roles.
- Successful reads record invalidation dependencies. Explicit unresolved waits
  name scheduling prerequisites. These are different records and lifetimes.
- Tock sees only its finalized requested logical input Regions. Discovery reads
  and wider cached storage must not leak into value-production bindings.
- Region/Coverage inputs preserve absence and never substitute manual or
  neutral values. Sequential inputs choose their declared neutral/manual
  fallback only on known absence. Pending discovery is not an uncovered result.

The useful parts of the existing work remain: typed messages, durable replacement,
gesture history, presentation-owned editing, immutable point arrays, sparse event
storage, modular neighbor lookup, and requested-window Tick replay. A finite
point-storage Region may remain. It does not define active curve intervals.

## 1. Complete requested-only Tock bindings

This is the first independent correctness feature. Audit and finish the path
from reverse input requirements to normalized logical Region selections and
the node-facing binding. Preserve full selected Regions, overlap, duplicate
ranges, and valid zero-event Regions. Derive node-visible positional coverage
from the explicitly selected Regions; do not expose additional stored Regions.
An empty resolved selection must remain empty.

Current implementation points:

- `BackgroundEvaluationTransaction::Impl::binding_selection()` fills
  `BackgroundEvaluationCallFrameSelection::binding_coverage` from required
  positions, while `binding_regions` selects intersecting whole input Regions.
  The two records need one consistent, explicitly resolved request.
- `BackgroundEvaluationCallFrame::seal()` assembles callback views from storage
  and checks propagated Region lists. This already implements part of the
  boundary; it is not a reason to assume every path meets the new invariant.

Verify equivalent results with cold storage and with unrelated extra Regions
already materialized. Cover continuous values, events, overlapping/duplicate
Regions, empty selections, and different requests in separate batch lanes.
Do not obtain requested-only views by clipping selected source Regions.

## 2. Implement suspendable propagation and its scheduler together


---

## `automation_and_suspendable_coverage_direction.md` original lines 308–424

input views or configuration references cannot outlive their selected version.

## 6. Tock input and output views

### 6.1 What is selected, and what may be read

A `tock_coverage()` invocation (and each `tock_coverage_batch()` lane)
receives **explicitly selected complete input Region views**, rather than the
union of anything already computed or cached. Its input `region()` (for
`RegionInputConfig`) or `regions()` (for `CoverageInputConfig`) is the readable
collection selected by reverse dependency planning and complete-Region
closure. Reading beyond that collection is invalid, even if backing storage
exists. A selected Region's full range is readable: a narrow requirement
intersecting a larger Region never turns the Region into a cropped pseudo-Region.
Overlapping or duplicate Regions remain separate, and zero-event event Regions
remain valid. Exploratory data reads made during propagation do not
silently enter the Tock invocation's selected input Regions.

An `IndexCoverage` is a positional set, **not** a Region collection. On the
intended Tock-facing API, `input.coverage()` (if exposed) is precisely the
positional **union of the Region views provided to that invocation**. It is a
convenience over their ranges, not a second, separately selected source of
truth. For example, provided Regions `[0,100)` and `[50,150)` retain two
independent values at index 75 while their coverage is `[0,150)`. The
reverse planner's potentially smaller or conservatively larger input
requirement is **not** this coverage and is not implicitly exposed as
`input.required_coverage()`.

The reverse propagation callback already knows the positions it required;
the ordinary Tock evaluator only needs the finalized readable input Regions.
If evaluation depends on a value obtained while planning, it must either
recompute the same pure determination from its selected inputs or consume an
explicit, versioned semantic result of planning; it must not take the
reverse requirement as a substitute for semantic input values.

An output's `region()` or `regions()` provides the complete writable Region
selection for the invocation. These ranges establish which output data must
be produced. The scheduler owns the original downstream demand, intersects it
with exact output coverage, and expands to complete output Regions as required
by the Region/Coverage production contract. `output.requested_coverage()` in
the existing API is a positional summary of selected output work, not a
source of unrelated content; its information can be derived from the selected
writable Region ranges once whole-Region closure is complete. The intended
minimal authored Tock interface therefore has no separate raw-request
accessor. Keep `region()`/`regions()` for writable data, and optionally
`coverage()` as a cheap positional union of exactly those writable Regions,
matching the input-side naming. The current output
`requested_coverage()` spelling can be retired or renamed during migration;
reverse propagation retains its own `output.required()` for demand planning.
Do not introduce `produced_coverage()` or an input-side
`required_coverage()` without a concrete case that cannot be expressed by
Region views and propagation.

### 6.2 Examples that test the minimum interface

| Eventual Tock node | Input-side information needed | Output-side information needed | What fails if that information is removed? |
| --- | --- | --- | --- |
| Constant or deterministic index-based generator | None | Writable `region()` or `regions()` and their ranges | Cannot know which finite output Regions to fill |
| Pointwise gain/copy | Input `region()` or `regions()` containing values | Output writable Regions | Cannot access input samples or distinguish separate Regions |
| Convolution / neighborhood filter | Input Regions including explicitly planned halo samples | Output writable Regions | Cannot evaluate at a boundary without the halo; input requirement itself need not be visible |
| Mixer with overlapping clips | Input `regions()` preserving Region identity and duplicate ranges | Output writable Regions | Positional `coverage()` alone loses contributions in overlap |
| Gap/mask-aware mapper | Input Regions; optional derived `coverage()` for set tests | Output writable Regions | Without the convenience union, it must compute the union itself; no expressive power is lost |
| Sparse point-event interpolator | Event `region()`/`regions()`, timestamps, payloads, and neighbor lookup | Output writable Regions | Cannot inspect control points outside the output window, even though the planner can select their finite Region |
| Selector whose input A chooses which part of B is needed | Selected A and B Region contents | Output writable Regions | Without suspendable reverse planning, the B requirement cannot be discovered; `input.required_coverage()` is not needed in Tock |
| Whole-clip normalizer | Full input Region defined by the clip's semantic boundary | Complete writable output Region | If planning supplies only a local sample window, output semantics change with demand; exposing a request accessor cannot fix incorrect selection |
| Event transformer preserving separate groups | Event `regions()` including Region membership/order | Writable event `regions()` | A coverage union alone discards group membership, multiplicity, and per-Region ordering |

These are tests of **information required by an evaluator**, not a claim that
all those nodes already exist. The first two columns follow the authored
`RegionInputConfig`/`CoverageInputConfig` and
`RegionOutputConfig`/`CoverageOutputConfig` distinctions: a singular Region
port uses `region()`, while a Coverage port uses `regions()`.

### 6.3 Request determinism and callback boundaries

What changes the Tock-visible records:

| Runtime change | `input.regions()` / derived `input.coverage()` | `output.regions()` / derived output coverage |
| --- | --- | --- |
| Different downstream demanded positions | May change **only because** a different set of complete input Regions is explicitly required | May change as a different set of complete output Regions is selected |
| Different forward-published Region ranges or existence coverage | May change when the reverse requirement now selects different input Regions | May change when the node's exact output Regions change |
| Changed point/sample/event values without a Region-range change | Region ranges may stay identical; readable contents/version change | Writable ranges may stay identical; recomputed results change |
| Conservative enlargement of an authored reverse requirement | May select more full input Regions; this must not change results on overlapping output positions | No automatic widening from the input storage cache |
| Warmed cache or unrelated materialization | **Must not change** selected Region descriptors or their union | **Must not change** the selected output work for the same demand/snapshot |
| Different compatible batching of demand roots | May change total explicitly selected Regions for the combined request | May change total selected output Regions; overlapping outputs remain semantically identical |

For example, if a reverse callback requests `[40,42)` from an input
whose selected source Region is `[0,100)`, the input `region()` grants access
to the **whole** `[0,100)` Region and the proposed `input.coverage()` returns
`[0,100)` as its union. The scheduler retains `[40,42)` internally as the
reverse requirement; it is not another Tock input accessor. If previous work
also happened to materialize `[200,300)`, that extra Region is not added to
the callback input.

Given an identical semantic/data snapshot, node configuration, and output
position, the produced value/event sequence must not depend on whether the
scheduler satisfied a requirement from cold computation, previous persisted
records, transaction-local materialization, or a different compatible batch.
An invocation's selected complete Regions must be determined from semantic
planning and declared output Region closure, not incidental cache contents.
Different output demands may select different sets of complete Regions, but
any overlapping produced positions must have the same semantic results.

This is stronger than merely repeating the same invocation and seeing the
same input spans. A node must not define normalization, interpolation, random
seeds, or boundary treatment from arbitrary demand grouping unless such
boundaries are stable parts of its authored semantics. `TockState` may
accelerate computation but cannot alter the results. Inputs are immutable
for the selected version and Region iteration order is deterministic.

The background scheduler may read/cache/materialize additional data to
satisfy independent consumers. Those extra data never become readable by a
Tock callback just because they happen to exist. A provided Region may
contain positions outside the smaller reverse-required set **because the
complete Region was explicitly selected**, and this is intentional.

### 6.4 Current implementation mismatch to resolve during migration

---

## `coverage_and_background_evaluation.md` original lines 1950–2168


After a candidate data version becomes published—including a transaction that
consumed recorder or Tick/persisted capture records—the executor may emit a coalesced
output change notification for subscribed external consumers.

Conceptually it contains:

```text
output identity
semantic version/revision
published data version
new IndexCoverage (or a coverage-changed indication)
changed IndexCoverage (sound may-change superset)
```

Notification is emitted from the executor publication/change boundary, not
recursively from individual callbacks. Presentation/UI code can react by issuing
ordinary random-access requests. The notification does not itself force
`tock/ephemeral` evaluation.

## 28. Executable-generation reconciliation and stable stored-output rebinding

JIT compilation and semantic invalidation are separate events. Producing a new
`CompiledGraph` generation does not by itself make any stable persisted output
stale or revoke its retention guarantee.

`BackgroundExecutor` reconciles old/new background generations using stable concrete-node/output
identity. Compatible persisted outputs rebind to the existing stable store rather
than copying data merely because machine code changed.

Important cases:

- tock/persisted with unchanged semantics preserves coverage and data;
- tock/persisted with changed semantics runs exact invalidation and rebuilds
  stale candidate ranges before publication;
- tick/persisted preserves finalized retained content across recompilation;
- ephemeral outputs have no persisted data to migrate; and
- access/retention changes rerun validation and reconcile storage conservatively.

Connection comparisons use stable semantic port identity rather than builder
handles, lowered node indices, or ORC-generation-local indices.

The executable local-continuity key is deliberately not a transitive graph hash. A
compiled background node carries:

- stable concrete-node identity and registered primitive identity;
- the exact provider-definition version;
- a shared typed-configuration ownership token retained only while the complete
  recursive configured-definition closure remains version-compatible; and
- its compiled background execution mode and semantic input/output port shape.

The handoff compares these facts with the exact active predecessor. A mismatch emits a
local-node invalidation root. Stable inputs are compared separately through normalized
incoming descriptions containing stable source-output identity, delivery mechanism,
type/conversion/projection facts, and target-channel mapping. A changed incoming set
emits an input root whose changed range is the union of the final predecessor input
coverage and successor input coverage. Existing forward propagation, rather than a
stored dependency fingerprint, carries both root kinds to all downstream outputs.

Definition-snapshot publication may preserve cached configured instances whose entire
recursive definition closure still matches. Replacing a definition invalidates all
cached parents which depend on it, including every instance of a broadly reused node
kind; unrelated definition publication does not invalidate them. A sample-rate change
is an explicit whole-plan local-semantic change because coverage callbacks observe the
rate. Initial graph loading has no predecessor and constructs fresh state.

## 29. Whole-project GraphJit integration

The generated project root remains a zero-input/zero-output Tick root. It does
not gain synthetic output ports for background evaluation or a project-wide tock callback.

`CompiledGraph` carries immutable metadata mapping internal coverage-planned ports to
generated background-evaluation component executors, persisted bindings,
advance-materialization plans,
and explicit recording-bridge capture/output identities. Static background-evaluation topology is
specialized during lowering rather than rediscovered for every request or block.

The generated background evaluation root also owns the static interleaving of node
invocations with transaction-supplied prepare/finalize hooks. The immutable plan maps
each hook and callback port to a compact runtime binding slot. Generated code calls
only those narrow hooks and consumes only those slots; it never receives an executor,
persisted-data-store or transaction pointer.

The generated Tick root remains a zero-project-port root but gains an explicit
invocation frame for dynamic background-derived bindings. GraphJit retains the
compile-time mapping from node ports to compact frame slots, and `RealtimeExecutor`
selects one immutable persisted-data snapshot plus any `TickMaterializationSnapshot`
at the root boundary. The frame contains views/bindings only, not storage-discovery
objects or ownership backdoors.

### Reuse whole-graph analysis products during lowering

Whole-project lowering reuses one SCC decomposition of the complete semantic
dependency graph to reject random-access input edges within a semantic SCC.
Separately, expand the background evaluation DAG through sequential input edges
of replayable tick nodes. Reject any unresolved replay dependency cycle; a valid
published persisted output is a terminal stored boundary for its covered requests.
Checking only labeled random-access edges is **not** sufficient for this second
check. Reuse the same stable indices and graph-analysis data; do not perform a
new reachability search per output.

SCC IDs, member ranges, condensation-DAG/topological order, dense node indices,
and reusable adjacency storage should be carried forward where useful. Background
component planning, same-Tick scheduling, liveness/storage planning, access/retention
lowering, and later batching/fusion/vectorization should consume those facts
instead of rebuilding equivalent graph views.

This is an efficiency rule, not a semantic requirement to collapse distinct graph
relations into one. Explicit-DAG validation, detach legality, same-Tick
dependencies, and complete semantic dependency relations answer different
questions. Detach validation may still require pre-feedback reachability. Share
indices/storage/traversal scratch where the edge relation actually matches.

The initial compiler should recompute SCCs from scratch for each graph compilation
rather than implement dynamic SCC maintenance. A complete Tarjan/Kosaraju-style
pass is linear, simple, deterministic, and expected to be negligible beside LLVM
lowering/optimization until profiling proves otherwise.

Execution ownership is internal to the `GraphExecutor` app module and split between
two actors. `RealtimeExecutor` owns canonical realtime `NodeStorage`, active/pending
realtime generations, pass-boundary activation and realtime queue producer endpoints.

`BackgroundExecutor` owns:

- stable canonical persisted-data stores/immutable roots for **all persisted
  outputs** (`tick/persisted` and `tock/persisted`), plus per-generation bindings;
- semantic versions plus immutable data versions and candidate/published snapshots;
- producer-specific pending queues and their consumer-side prefix ownership plus pre-sized selection descriptors;
- transaction workspaces;
- exact forward mutation/change transactions;
- reverse demand/Tock transactions;
- completion scheduling for candidate Tock/persisted outputs and incorporation of
  selected Tick/persisted queue prefixes; and
- external Random Access request/change-notification lifetime.

`AsyncCapacityManager`, also owned by `GraphExecutor` as runtime infrastructure, owns
non-realtime provisioning/recycling of queue blocks. No mutable state is jointly owned
between the realtime and background actors.

## 30. Deliberately open implementation/tuning choices

Implementation choices include record backing, range-tree/directory representation,
the concrete callback-boundary/epoch mechanism used for audio-safe reader pins,
transaction-workspace reuse, queue block sizes and capacity-watermark strategy,
worker scheduling, cancellation granularity, and post-correctness SIMD/fusion/
value-specialization cost models. None of these may weaken the authored persisted
retention obligation or introduce audio-thread tock execution.

The following are **not** implementation choices: static constexpr concrete node
port schemas; independent input access/output production/output retention; explicit
opt-in replayability with contextual dependency validation; no implicit recording of
unreproducible Tick/ephemeral sources; one canonical persisted-data read abstraction
for Tick/persisted and Tock/persisted outputs; background-only Tock and propagation;
published/materialized-snapshot-only Tick-time Random Access in the preliminary
implementation; stale-data-as-is/missing-position-per-input-neutral Sequential playback;
subset-first storage requirement inference before storage coalescing; no persisted
data eviction except loss of covered positions; exact output coverage and sound
conservative forward invalidation; suspendable value-dependent forward/reverse
planning when needed; explicitly selected full Tock input Region views; fixed
capture-prefix transactions; callback-boundary-safe
capture-block reuse; transaction-wide coverage/data/frontier commit; compiler-owned
materialization scheduling; narrow transaction-local generated-root hooks with no
executor/runtime-owner backdoor; legal-size replay invocation splitting; and no
unresolved Random Access evaluation cycles.

## 31. Static validation

Target node/package validation rejects non-static or non-constexpr concrete port
schemas at **every** construction path (including internal builder paths), malformed
authored `State` in either execution domain, conflicting callback/production declarations, invalid event
capacity declarations, and invalid replayability trait declarations. The planned batch
API additionally rejects ambiguous authored shapes: a primitive authors exactly one
primary form across `tick()`/`tick_block()`/`tick_block_batch()` or
`tock_coverage()`/`tock_coverage_batch()`; every output matches that selected domain;
and skip plus each authored
propagation direction choose one supported synchronous scalar, coroutine scalar,
or synchronous batch form as specified in
[Batched Node Callback Direction](../batched_node_callbacks_direction.md).

The following is a source-status checkpoint and retains legacy type names:
the checked-in replayability validation accepts `tick()`, `tick_block()`, or
`tick_block_batch()` through the normalized `do_tick_block()` operation. It rejects
`State`, `TockState`, Random Access inputs, nonzero port history or latency, and
nonzero internal latency. This is an implementation checkpoint, not the target
semantic rule. The target replayability trait removes the prohibition on Random
Access inputs. It retains the deterministic, side-effect-free, state, history, and
latency restrictions. Front 2 removed the callback-shape special case. The replay
contract still requires pure deterministic evaluation under a fixed version.

Whole-project GraphJit validation resolves per-channel connections and semantic
SCCs, derives contextual replayability and exact available coverage, rejects
unreproducible tick/ephemeral -> random-access demand unless an explicit recorder
intervenes, and rejects unresolved background evaluation cycles. A Tock ->
Random Access consumer requires an immutable addressable materialization; persisted
outputs use canonical persisted data, while ephemeral storage lifetime depends on the
consumer callback context. Explicit recorder bridges must have their valid authored
input/output shape; any realtime-produced queue plan must have realtime-safe pre-provisioned
capacity and pass-boundary-safe ownership/reclamation. Unsupported event-
replay contracts must be rejected, not silently treated as sample replay. Errors
identify the specific source channel, input, dependency
path, or offending node where practical.

Plan validation additionally requires every runtime background binding slot to have
one compatible producer, every planned materialization input/output index to be in
range, every before/after-node operation placement to respect background dependency
order, every persisted binding to have stable or explicit generation-local identity,
and every replay invocation to respect the compiled maximum block size. Missing facts
are compiler-plan errors; the executor must not repair them by rediscovering topology.

Transaction/runtime tests cover failure during F, R, authored Tock, replay,
materialization, candidate validation and publication; every case must leave committed
coverage, the published snapshot and selected queue-prefix ownership unchanged. They also cover
stale-base rejection, old-or-new whole-snapshot reader visibility, complete exact-range
validation, stable and anonymous output identities, authoritative empty event chunks,
deterministic equal-time event ordering, missing Sequential-input neutral values, and
the prohibition on audio-thread Tock/allocation/blocking/final-owner reclamation.

## 32. Implementation landing order


---

## `dsp_execution_storage_glossary.md` original lines 391–539

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
version. Its positional writes derive zero or more maximal contiguous Regions from
`IndexCoverage`; overlapping and touching writes merge. It therefore has Coverage
shape and cannot directly satisfy a Region input.

## Background evaluation and publication

### Background evaluation transaction

A **background evaluation transaction** is one coherent unit of invalidation-root
and demand-root collection, forward/reverse propagation, and required Tock/replay
evaluation against one coherent semantic/data-version view. When persisted state is
changed, the same transaction completes and publishes the corresponding candidate.

Forward/reverse propagation produces **prepared coverage**. It is not authoritative
until final commit promotes that coverage together with the successor persisted-state/
data snapshot and the domain-specific release of selected producer-queue prefixes.
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

### Data version

A **data version** identifies one immutable published persisted-data root produced by
a completed background evaluation transaction. Use the conceptual pair
`(semantic_version, data_version)`.

### Candidate / published

A **candidate** is a not-yet-published successor being completed under a target
semantic/data version. **Published** data is the immutable version externally or
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

It is not the owner of a Tock primitive's `State`, background workspaces,
dynamically sized persisted-output data, or recording capture backlogs. Tock
`State` is separately owned background acceleration state and may use dynamic
allocation. Tick and Tock primitives author the same optional nested `State`
name, but its storage and lifecycle capabilities follow the callback family.
Tock state may be migrated on the background worker after prior evaluation has
finished, or initialized fresh when migration is unsafe.

### Persisted data store, sample region, and event chunk

The **persisted data store** is the canonical retained/versioned owner for every
persisted sample or event output. Tick and Tock production feed the same store and
publish one coherent immutable root; production mode does not create another retained
read path.

A **persisted sample region** is one immutable contiguous sample payload for an
explicit `GlobalIndexRange` and channel layout. A **persisted event chunk** is one
immutable explicit range plus its complete ordered event set, which may be empty.
Neither record is a fixed-width page, and neither record boundary is determined by the
realtime root block size.

For a Tock Region or Coverage output, the persisted store also keeps the ordered
logical list of these records. That list preserves overlap and exact duplicates.
Physical range-index records and signed-seam slices do not define the list exposed to
node callbacks.

Validity is version-specific. An older readable root may remain pinned while a
successor candidate is incomplete or stale. Backing storage may be RAM,
mmap/file-backed storage, immutable chunks, compression, or another retained-data
backend without changing the sample-region/event-chunk semantics.

Do not call an ephemeral intermediate persisted data.

### Transaction-local materialization

A **transaction-local materialization** is an ephemeral concrete representation
created for one background evaluation transaction and reusable by consumers within
that transaction.

For a Tock/ephemeral or replayed Tick/ephemeral result feeding a background-only
Random Access consumer, the preliminary implementation may use a
**transaction-local materialization** so the consumer receives the
required addressable representation. If Random Access occurs during Tick execution,
the addressable representation must instead be materialized and selected before the
callback. Replayable Tick work may avoid or fuse materialization where the consumer
contract permits it. A transaction-local materialization is not persisted data and
carries no retention guarantee beyond the transaction/readers
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
view the appropriate pinned persisted sample regions or event chunks directly.

### Tick materialization snapshot

---
