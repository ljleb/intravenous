# Coverage Planning Before Suspendable Propagation (2026-11-15)

_Status: historical, non-normative. Source: `docs/coverage_and_background_evaluation.md` (the pre-edit staged repository snapshot), with further passages from `docs/batched_node_callbacks_direction.md` and `docs/sequential_port_storage_planning.md`. Preserved 2026-11-15 because the accepted [suspendable propagation and automation direction](../automation_and_suspendable_coverage_direction.md) replaces value-blind, single-pass dependency planning and clarifies directed event projections. The previous wording is retained below verbatim; it describes the earlier intended contract, not the new one._

## Previous coverage sections (verbatim)

## 8. `propagate_forward_coverage()`

The forward dependency callback answers:

> Given this node's current random-access-input coverages, reported changed
> random-access-input regions, current semantic configuration, sample rate, and any
> local semantic change cause, what are the resulting exact computed-output coverages
> and what sound output-region superset contains every value that may have changed?

Conceptually:

```text
F(node): input coverage + changed input regions + semantic configuration + sample rate
      -> output coverage + affected output regions
```

It runs in forward graph direction. Incoming may-change regions from all random-access
inputs and all invalidation roots belonging to the batch are accumulated/unioned
before the node is visited. **Each implicated node is visited at most once by the
forward phase of one background evaluation transaction.** A node may still have many disjoint changed
regions and multiple changed inputs in that one callback invocation.

For every node that declares a computed tock output, an explicit forward-coverage
implementation is **mandatory**. Under the planned batch API that implementation may
be authored in scalar or batched form and is normalized to both forms by the traits
layer. The output Region-range list is semantic and exact, and its derived
`IndexCoverage` union is exact; there is no generally correct fallback that can invent
the list. A framework may still
generate trivial glue when exact coverage is mechanically declared by another
static facility, but it must not silently preserve old/empty coverage or substitute
a conservative superset.

A node's affected-output region may be conservative. Under-reporting is a correctness
error; over-reporting is legal and only increases invalidation/evaluation work. The
callback is value-blind in the same sense as reverse planning: it may use coverage,
semantic configuration, sample rate and deterministic structural metadata, but it
does not evaluate transported input values merely to discover a tighter dependency
footprint. If a data-dependent address mapping cannot be bounded more tightly from
structural information, reporting the complete potentially affected output coverage
is valid.

A replayable Tick node with only mechanically pointwise dependencies needs no authored
forward or reverse coverage callback: GraphJit generates the same-position temporal
mapping from its static input/output dependencies. A replayable Tick node with Random
Access dependencies whose footprint is not mechanically derivable participates in the
same propagation framework and must provide the required dependency mapping; those
callbacks remain background-only and do not add a Tock output. A tick-only node with
persisted output is the recorded boundary for finalized data; it does not acquire a
`tock_coverage()` implementation. A Tock/persisted output remains an ordinary computed
output with exact forward-coverage semantics and background `tock_coverage()`
materialization; it is not a realtime recording output.

Tock-owned `State` is **not** visible here. Forward dependency semantics must not vary
according to memoization or acceleration history.

Forward and reverse propagation are directional dependency queries, not inverse
functions.

For an FIR-like transform of support `L`, for example:

```text
input changed [a,b)
    -> output may have changed [a,b+L-1)
```

while reverse demand maps output requirements to a different input region.

---

## 9. `propagate_reverse_coverage()`

The reverse dependency callback answers:

> Given exact covered output regions that must be evaluated by this node in the
> background, which covered regions of its random-access inputs must be available
> to compute them?

Conceptually:

```text
R(node): requested output coverage + semantic configuration + sample rate
      -> required input coverage
```

It runs in reverse graph direction. Requirements reaching a node through multiple
downstream paths and demand roots are accumulated/unioned before the node is
visited. **Each implicated node is visited at most once by the reverse phase of
one background evaluation transaction.**

For `tock/ephemeral`, the output requirement is the exact covered demand. For
`tock/persisted`, candidate completion selects exact stale or absent covered ranges,
and those ranges become the requirements supplied to reverse planning.

Persisted output data is a reverse-propagation boundary only when it is valid
for the `(semantic_version, data_version)` pair selected by the batch:

- compatible `tock/persisted` data for the selected target/base version pair
  satisfies that requirement and stops reverse propagation through that region;
- a stale or absent `tock/persisted` candidate range does **not** stop reverse
  propagation merely because older stored data still exist. The exact covered range
  becomes a materialization requirement and reverse planning
  continues through its producer; and
- `tock/ephemeral` owns no persisted result and therefore never forms a retained
  reverse cut.

Thus retention and semantic compatibility are deliberately distinct. Never
dropping old immutable persisted data does not make those data current for a
new semantic version.

After the callback reports input requirements, every requirement is clipped to the
input's exact coverage.

The callback is **value-blind**. It may inspect coverage, semantic node
configuration, sample rate, and other deterministic structural metadata, but it
must not inspect random-access input data values to discover a second-stage
dependency footprint. If exact addressing depends on a random-access control input,
the callback returns a conservative superset derivable without reading that
signal, up to the complete potentially relevant input coverage. A future staged
value-dependent dependency facility may relax this rule without changing the
initial ABI.

The callback may be omitted where a conservative rule is mechanically
known, for example by requiring all covered regions of every random-access input.
Output-only Tock sources have nothing upstream to request and therefore require
no reverse callback.

For a tock-produced output needed by sequential playback, reverse planning and
all required computation run off the audio thread. The audio thread only reads
the already published/materialized result; an unavailable position uses the sequential
consumer's `neutral_value`.

Tock-owned `State` is **not** visible here. Dependency requirements may not depend on
memoization history.

---

## 10. Tock coverage evaluation callbacks

`tock_coverage()` is the checked-in one-node background-evaluation callback for
Coverage outputs. Region production uses the corresponding finite-region Tock
operation. The planned
`tock_coverage_batch()` form evaluates several compatible nodes as one compiler/runtime
operation; each lane still carries the exact scalar context for its own node. A
node's requested `IndexCoverage` is never merged with another lane merely because the
nodes are batched.

Within one background evaluation transaction, reverse planning first finishes
accumulating the final requested coverage for every implicated output. Forward
evaluation then executes the normalized Tock operation **at most once per implicated
node for the whole transaction phase**, either as a scalar callback or as one lane of
a batch callback. Requests for several outputs, several disjoint regions, and several
downstream consumers are therefore coalesced before that node participates in the
operation.

The callback receives only work that needs computation for the target operation:

- a `tock/ephemeral` output receives exact demanded covered regions; and
- a `tock/persisted` output receives selected exact stale/missing ranges, each already
  intersected with exact output coverage.

It never receives uncovered positions merely because they share storage with a
selected range.

The callback:

- sees random-access inputs and computed tock outputs;
- sees the project sample rate for the semantic version being evaluated;
- operates on covered global sample/event regions;
- cannot request random-access inputs outside their coverage;
- cannot depend on mutable or unavailable live tick values; finalized persisted
  tick outputs and contextually replayed tick values are valid sources through
  random-access inputs;
- cannot depend on Tick-owned sequential `State`;
- may use its own background-owned `State` solely for non-semantic
  acceleration/memoization; and
- must produce observable results independent of background request order and the
  contents/history of that `State`.

The background-owned `State` of a Tock primitive has one deliberately narrow
meaning:

> It may change the pace or implementation strategy of `tock_coverage()`, but it
> may not change output values, output coverage, reverse requirements, or any other
> observable semantics.

It is separate from packed realtime `NodeStorage` and may use ordinary dynamic
allocation. `initialize(InitializationContext<Node> const&)` may establish
configuration/resource-dependent acceleration, while cache contents derived from
current inputs/coverage belong in background `tock*()` work.

A correct node must produce the same results from freshly initialized Tock `State`.
`BackgroundExecutor` may serialize, duplicate, reset, or migrate compatible state
without changing observable semantics. An optional `move(MoveContext<Node> const&)`
can retain compatible caches and must invalidate entries made stale by the
successor's configuration, inputs, or semantic version. Migration occurs only after
predecessor background work using that state finishes; otherwise fresh
initialization is required. One mutable Tock `State` instance must not be mutated
concurrently by overlapping evaluations unless its own representation makes this
safe. None of this introduces audio-thread locks or ephemeral-output exceptions.

Successful tock completion is transactional. For samples, every requested covered
sample/channel of every requested output is completely initialized. For events,
the callback emits the complete event sequence for every requested covered region;
zero events is a complete result. Failed, cancelled, or superseded work commits no
partial output as valid.

Within each event output Region, events are emitted in nondecreasing global timestamp
order. Different Regions own independent event sequences and impose no write order on
one another. Equal-timestamp producer-local order is preserved within each Region.

A single callback remains preferable to mandatory per-output callbacks because one
Tock primitive may share useful work among several Tock outputs. **Tock and its propagation
callbacks are background-only.** Ephemeral retention never grants an audio-thread
live-pull exception. Eligible pure `tick()` nodes are different: their already-
generated `tick_block()` implementation may execute during background replay.
That replay invocation remains Tick semantics and does not create a Tock callback.

The scalar name deliberately does not contain `batch`: `tock_coverage()` evaluates
one node. Executor-level transaction batching across demand roots is a separate
concept. The planned `tock_coverage_batch()` ABI evaluates **multiple compatible
nodes** simultaneously, with separate coverage, bindings, and optional acceleration
state for every lane; see
[Batched Node Callback Direction](./batched_node_callbacks_direction.md).

---

## 11. Batched requested coverage and stored-candidate completion

Background evaluation is organized around **batched demand**, not one traversal per
read or physical persisted-data record. A batch first collects all demand roots that
are allowed to participate in that transaction, unions convergent requirements, then
runs one reverse phase and one forward evaluation phase.

One background evaluation transaction uses one coherent environment: one target/base
`(semantic_version, data_version)` pair, one sample-rate/configuration view, one set
of persisted-output bindings, and independently selected finite prefixes from every
relevant pending producer queue. A publication linked after a queue's remembered
terminal block is not part of that workload. There is no atomic cross-queue snapshot
requirement.

Pure stored reads may need neither R nor T; a pure invalidation batch may defer R/T;
and a mutation-plus-fetch operation may run F then R/T before returning results. The
batching invariant constrains how work is coalesced when a phase is present, not which
phases every caller must execute.

There are two closed classes of **external demand roots**:

1. **application/UI random-access fetches**, such as requests for one or more
   requestable outputs/ranges; and
2. **advance materialization demand** for sequential playback, scheduled in the
   background for positions a future `CompiledGraph::tick_block()` invocation may
   read. Audio execution never synchronously turns a miss into a new demand
   transaction or runs tock.

The host protocol need not expose these as one-request/one-batch operations. One
application request may contain multiple fetches, and an update request may also ask
for results after the update. The executor normalizes the request into batched
mutation and demand roots.

There is also one internal materialization class: exact `tock/persisted` coverage
that is absent or semantically stale in the private candidate but must be completed
before its target version can publish. This is positional demand; it is unrelated to
the physical boundaries of existing sample regions or event chunks.

### Demand-driven access

For requested `tock/ephemeral` sink outputs, the batch:

1. intersects every explicit request with exact output coverage;
2. unions all covered requirements for each output;
3. reverse-propagates the coalesced requirements through ephemeral tock producers
   and contextually replayable tick nodes until reaching compatible published
   persisted-output boundaries or source nodes;
4. evaluates implicated nodes in forward dependency order, at most once per node;
   and
5. returns or forwards the requested materialization through direct transaction-local
   views or bounded materialized storage appropriate to the consumer.

A request for a published persisted output reads the requested subset from the
canonical persisted-data snapshot selected for the transaction. A Tock/persisted
candidate may require background reconstruction before a successor version can
publish. Tick/persisted data reaches the same store through the realtime-produced
queue/finalization handoff and needs neither a Tock callback nor an explicit recorder.

### `tock/persisted` candidate completion

Forward invalidation adds exact stale/missing positional ranges to the private
candidate's completion demand. Those internal roots are unioned with explicit demand
roots for the same transaction. To make the candidate publishable, the executor:

1. gathers every exact stale or absent `tock/persisted` range required by the target
   output coverage;
2. unions those internal roots with explicit demand roots;
3. runs one reverse-order pass, coalescing downstream requirements before each node
   and stopping where compatible published persisted data satisfies a range;
4. runs one forward-order evaluation pass, invoking each implicated tock node at most
   once for the consolidated batch and replaying eligible Tick nodes through their
   imported generated wrappers; and
5. applies exact-range sample-region/event-chunk overwrites or erasures to the private
   candidate and publishes only after all required stored state is complete.

Conceptually:

```text
all invalidation roots for batch
        |
        v
one forward dependency pass (F)
        |
        v
sound may-change ranges + exact persisted completion demand
        |
        +-------------------------------+
        |                               |
        |                    explicit UI/materialization demand roots
        |                               |
        +---------------+---------------+
                        |
                        v
              union all demand roots
                        |
                        v
one reverse dependency pass (R)
  each implicated node <= 1 reverse callback
                        |
                        v
one forward background evaluation pass (T)
  tock callback <= 1/node; replayable tick wrapper per required block
                        |
                        v
complete candidate records / transient results
                        |
                        v
publish one coherent persisted-data root
```

Compatible published persisted data may satisfy an upstream requirement without
further traversal. Stale retained data belongs to the previous semantic version and
cannot satisfy the candidate; its exact required range remains in the transaction.
Tick/persisted candidate records are filled from the fixed selected queue prefixes
instead of traversing the live Tick producer.

Complete stored publication does **not** require every transient intermediate or all
replacement records to coexist in memory at once. `BackgroundExecutor` may reuse
bounded transaction storage while honoring the semantic batch. Such chunking must not
split a node into multiple `tock_coverage()` invocations for one logical batch; if a
workspace cannot hold consolidated requirements, lowering must provide a bounded
streaming representation that preserves the one-callback-per-node batch semantics.

---

## 12. Batched forward invalidation phase

A forward invalidation phase begins from the closed invalidation-root set in
section 7. All roots assigned to one background evaluation transaction are installed before
forward traversal starts.

The evaluator conceptually performs:

```text
all node-state / Tick-capture / graph / sample-rate invalidation roots
        |
        v
start from affected nodes/ports
        |
        v
walk nodes once in forward dependency order
        |
        v
union reported may-change input regions + current input coverage
        |
        v
propagate_forward_coverage() at most once for this node
        |
        +--> publish exact continuous Region list or event coverage
        |       |
        |       `--> derive its positional union and added/removed changes
        |
        `--> publish sound may-change output regions
                |
                +--> mark exact stale tock/persisted candidate ranges locally
                |
                `--> union semantic may-change regions into downstream accumulators
```

Fan-in never causes repeated callbacks within the batch. Every upstream path that
can reach a node in forward order has contributed to that node's per-input change
accumulator before its callback runs. Fan-out distributes the callback's consolidated
output changes into downstream accumulators.

Forward propagation continues **through** `tock/persisted` computed outputs. A stored
output is not an invalidation cut: keeping older persisted data alive does not make
downstream semantics current for a new candidate version. Physical record boundaries
do not widen semantic may-change ranges.

The forward phase itself does not recursively evaluate nodes. After it finishes,
all exact stale/missing persisted ranges are known and can become demand roots for
the batch's R/T phase. The executor may defer that materialization, but a candidate
containing `tock/persisted` outputs cannot publish for its target version pair until
its required output coverage is complete.

## Previous batch callback statement (verbatim)

> In one applicable transaction phase, each implicated node participates at most once
> in the normalized operation, whether that operation is emitted as a scalar callback
> or as one lane of a batch callback.

## Previous event conversion section (verbatim)

## Event conversions are directional semantic conversions

Event conversion planning must fail when producing the target data would
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

This rule also removes the old conversions which generated a second event at
`t + 1`; current built-in conversions never invent a later timestamp. Tick-window
validation nevertheless checks converted events at the point they are
emitted, so future conversion additions cannot silently escape the legal
window.



## Previous input-Region and value-dependent discovery wording (verbatim)

Source: `docs/random_access_port_data_direction.md`, pre-edit 2026-11-15. The selected-only Tock input binding and suspendable forward discovery contract are now in [the replacement direction](../automation_and_suspendable_coverage_direction.md).

A consuming callback still receives only the
Regions that intersect its own input requirement. A node
whose Region ranges depend on data values must determine and retain those ranges
before forward propagation; it cannot discover the output shape while writing the
output values.
