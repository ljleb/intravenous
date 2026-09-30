# Realtime / Background Execution And Provisioned Queues

_Status: normative runtime direction for `GraphExecutor` execution ownership, realtime/background handoff, asynchronous capacity provisioning, and hot reload._

## 1. App-module boundary versus execution-actor boundary

`GraphExecutor` is one application module.

Internally it owns separate execution actors and runtime infrastructure:

```text
GraphExecutor
|- RealtimeExecutor
|- BackgroundExecutor
|- AsyncCapacityManager
`- ProvisionedQueue<T> instances / internal handoff channels
```

The actors are separate because they have different thread ownership, latency rules,
and mutable state. They are **not** separate app modules.

Application modules may have bidirectional static relationships. The separate rule in
[event_propagation_tree_constraint.md](./event_propagation_tree_constraint.md) is that
one dynamic app-module event propagation must form at most a tree. Internal
`RealtimeExecutor <-> BackgroundExecutor` handoffs are not app-module event
propagation and therefore do not appear in those trees.

`RealtimeExecutor` and `BackgroundExecutor` share no mutable executor state. Large
cross-actor transfers use pointers/ownership transfer to heap-allocated or provisioned
objects rather than copying payloads.

## 2. `GraphExecutor` owns one logical execution generation

One successful `GraphJit` result defines one logical execution generation. That
generation has a realtime half and a background half, but callers stage it once:

```text
ProjectGraph
    |
    | CompiledGraph generation N+1
    v
GraphExecutor
    |
    +-- prepare BackgroundExecutor half first
    |
    `-- prepare matching RealtimeExecutor half
```

The background-first ordering is internal executor implementation. `ProjectGraph`
does not address the internal actors separately.

Background preparation must complete before the matching realtime successor can become
activatable. Preparation includes, as applicable:

- generation-specific producer/consumer queue endpoints;
- stable logical identity and route bindings;
- persisted-state migration metadata;
- prepared generation-cutover storage/object(s);
- required queue capacity provisioning; and
- any other resources required so the later realtime boundary performs no dynamic
  allocation.

A staged generation is not active merely because preparation completed.

## 3. Realtime actor

`RealtimeExecutor` owns realtime-thread mutable state, including:

- active/pending realtime realizations;
- canonical realtime `NodeStorage` and state migration;
- whole-pass transition/steady activation;
- the active immutable persisted-state/page version selected for the current pass;
- a pending compatible persisted-state version, when background has published one;
- realtime invocation/materialization bindings; and
- producer endpoints for recording, Tick/persisted output and other dynamically
  accumulating realtime-produced inputs to background execution.

An in-progress realtime pass never changes generation or persisted-state version.
Pending replacements become active only at legal pass boundaries.

Realtime execution must not allocate dynamically, wait for the background worker, or
resize cross-thread collections.

## 4. Background actor

`BackgroundExecutor` owns:

- its worker thread;
- staged/current background generation halves;
- ordered generation cutovers actually published by realtime;
- background-only mutable evaluation state and optional `TockState`;
- canonical persisted-page/recording state and immutable publication roots;
- producer-specific pending queues;
- background coverage/propagation workspaces and transaction-local candidates; and
- exact workload selection and commit.

Incoming data may wake the worker, but it never modifies work already selected for the
current background operation.

The worker selects one precise finite workload, executes it, commits or rejects it,
and then selects again from the latest pending state.

## 5. Provisioned SPSC queues

Dynamically accumulating cross-thread inputs use producer-specific SPSC queues backed
by power-of-two fixed-capacity blocks.

A queue is not required to expose a logical `size()`, logical indices, or a contiguous
resizable array. Its useful semantics are block ownership and prefix release.

A typical physical queue is a linked sequence of stable blocks:

```text
[first pending] -> [block] -> [block] -> ... -> [published tail]
```

The producer obtains already-provisioned blocks, constructs one or more blocks
privately, and publishes a complete initialized chain. The consumer processes/release
prefix blocks in order.

Power-of-two block capacities are normative for the generic implementation because
they simplify layout/alignment and any block-local indexing.

## 6. Private chain construction and cheap publication

The producer owns a block exclusively before publication. It may initialize payload,
metadata, `used` count and private links with ordinary non-atomic writes.

Conceptually:

```text
producer-private

[A] -> [B] -> [C] -> null
```

Only the handoff that makes the complete chain visible to the consumer requires
publication synchronization.

The producer passes at least:

```cpp
struct ProducedBlockChain {
    Block* first;
    Block* last;
};
```

The consumer side connects the already-built chain to that producer's pending queue
with one cheap pointer publication (or installs the first pointer when the queue was
empty). The producer already knows `last`; the consumer does not traverse the incoming
chain merely to append it.

The precise atomic representation is an implementation detail, but the guarantee is:
if the consumer can observe the published chain link/head, every block in that chain is
fully initialized and safe to read.

There is no per-entry atomic publication requirement for the current application.
Audio/event production has a natural realtime-pass/block publication boundary.

## 7. Direct production into provisioned blocks

Where layout permits, realtime audio/event output should be written directly into
capacity-manager-provisioned queue blocks.

Preferred path:

```text
AsyncCapacityManager provisions block
            |
            v
RealtimeExecutor / generated DSP writes payload directly
            |
            v
producer seals private chain
            |
            v
BackgroundExecutor pending queue
```

Avoid a second realtime-owned recording buffer followed by a copy into queue storage
when the queue block can be the original destination.

This reduces both memory footprint and memory traffic.

## 8. Independent queue pinning

`BackgroundExecutor` may consume many producer queues. Each queue is pinned
**independently**.

Before one background workload starts, background discovers and remembers one finite
terminal block for every queue it chooses to consume:

```text
queue A: A0 -> A1 -> A2 -> A3 -> ...
                      ^ selected last

queue B: B0 -> B1 -> B2 -> ...
                ^ selected last
```

Later publication may extend either queue. It does not change the selected workload.
During execution, background stops at the remembered terminal block; it does not keep
following `next` until it happens to see null.

There is intentionally **no atomic snapshot relationship across different queues**.
If queue A and queue B are changing while background selects work, newly published
items may land in either the current or the next background pass independently.

This is an explicit application assumption, not an accidental race to be fixed.

If a future feature requires atomic visibility or ordering across two producer queues,
implementation must stop and that requirement must be designed explicitly. Do not add
implicit global locking, a shared queue size, or cross-queue snapshot synchronization
to the generic queue primitive.

If two pieces of information are semantically required to become visible atomically,
they should normally be one published object/chain or carry an explicit version/identity
that defines their relationship.

## 9. Work selection is immutable

Once `BackgroundExecutor` has selected a workload, later changes cannot modify it.

This applies to:

- later queue appends;
- later desired background ranges;
- later node/background changes;
- later compiled generations; and
- later persisted-state publications.

Those become inputs to later workload selection.

The background worker is therefore allowed to run without holding a lock against
producers while performing expensive computation.

## 10. `AsyncCapacityManager`

`AsyncCapacityManager` is runtime infrastructure owned by `GraphExecutor`, not an app
module. It owns the non-realtime process/thread that provisions and recycles queue
blocks.

Each producer declares its own capacity requirement. The current policy vocabulary is:

```text
C = maximum producer burst that must fit without new provisioning
L = low free-capacity watermark
H = refill target
G = allocation/segment granularity
```

`C` is a producer-derived structural bound. `L/H/G` are operational provisioning
policy. Normally `C << L < H`.

When available producer reserve falls below `L`, the manager provisions/recycles enough
whole blocks/slabs to move back toward `H`, rounded according to `G`.

Only producers advertise these requirements. Consumers do not need to know queue
capacity policy.

The manager may track its own allocation/release accounting and tail pointers. Those
are capacity-management state, not queue semantics exposed to producer/consumer users.

## 11. Ownership cycle

Block ownership must be explicit. A block follows one direction:

```text
AsyncCapacityManager
        |
        | assigns ready capacity
        v
producer reserve
        |
        | producer acquires
        v
producer-private
        |
        | publish complete chain
        v
BackgroundExecutor pending/selected work
        |
        | successful release
        v
AsyncCapacityManager
```

At each point one subsystem owns mutation rights.

The background consumer does not splice completed blocks directly into a realtime
producer's reserve. It releases them to the capacity manager; the manager later
reassigns/recycles them.

Likewise, a realtime producer never deletes or reallocates published blocks.

Heap-owned objects referenced by queue entries need equally explicit lifetime transfer.
A pointer entry must define whether ownership transfers with publication or whether a
separate immutable/shared lifetime protocol applies. Reclamation must never fall onto
the realtime thread accidentally.

## 12. Recording semantics on the queue transport

Recording is one use of this generic transport. There is one non-configurable recording
semantic model:

```text
ordinary write
    -> publish overwrite data

no write
    -> publish nothing
    -> preserve previously recorded data

write_void()
    -> publish explicit authoritative erase
```

Recording is RAM-backed, overwrite-at-addressed-position semantics. It does not append
or offset subsequent data and has no generic recorder arm/start/stop policy.

A written recording block is never intentionally dropped. Failure to obtain required
preprovisioned capacity is a recording/resource failure.

Where possible, recorded sample/event payload lives directly in queue blocks from the
start rather than being copied into a second handoff representation.

## 13. Paired-generation hot reload

Hot reload treats realtime and background halves as one logical generation.

### 13.1 Staging

`ProjectGraph` supplies generation N+1 once to `GraphExecutor`.

`GraphExecutor` internally:

1. prepares the N+1 background half first;
2. prepares generation-specific queues/routes/migration/cutover resources;
3. ensures required producer capacity is available;
4. only then prepares/stages the N+1 realtime half.

If background preparation fails, the realtime successor is not staged.
If realtime preparation subsequently fails, no cutover occurs. A staged-but-never-
activated background successor is not semantically active and may be reclaimed or
superseded off realtime.

### 13.2 Authoritative cutover

The actual generation transition is realtime-authoritative and happens at a legal
realtime pass boundary.

The boundary performs, in order:

```text
1. publish all final generation-N producer chains from the completed pass
2. publish the already-prepared N -> N+1 cutover to BackgroundExecutor
3. swap RealtimeExecutor active generation N -> N+1
```

These synchronous boundary operations are bounded pointer/state operations and require
no dynamic allocation.

The cutover publication does **not** synchronously run background evaluation. It merely
makes the prepared transition visible to the background worker.

### 13.3 Old generation queues close

After the N -> N+1 cutover, no generation-N producer queue can grow again. Every old
queue therefore has a finite tail.

The background worker may still be:

- finishing work already selected under N;
- draining remaining closed N queues; or
- preparing the N -> N+1 persisted-state migration.

All such work remains interpreted using generation N.

Only after required N work is complete does background apply the prepared migration and
begin interpreting N+1 queue data under N+1.

This does not require a global atomic snapshot across old queues. Closure makes each old
queue finite; background may drain them independently according to ordinary workload
selection rules.

### 13.4 Removed producers and destinations

Stable logical identity, not generation-local slot/index reuse, determines continuity.

If both producer and destination survive, old-generation writes are drained under N,
retained destination state migrates, and N+1 writes continue normally.

If the producer disappears but the destination survives, remaining N writes are
drained and the destination's final state migrates. No N+1 writes arrive from the
removed producer. For recording, absence of later writes preserves retained content.

If the destination disappears, remaining N writes are still valid N work. The
destination is retired during N -> N+1 migration. N+1 has no route to it.

If both disappear, remaining N work may finish and both identities are then absent from
N+1.

Replacement/install order must not change these outcomes.

### 13.5 Persisted-state publication during lag

Background-published immutable persisted-state versions identify the generation they
are compatible with.

If realtime already runs N+1 while background finishes a final N result, that result is
background input to the prepared N -> N+1 transition. It must not become the active
persisted view of realtime N+1 directly.

Once background has a coherent N+1-compatible immutable version, it publishes that
pointer internally to `RealtimeExecutor`. Realtime stores it pending and activates it
only at a later pass boundary.

### 13.6 Ordered cutovers

A successor that was staged but never activated may be superseded without creating a
semantic generation boundary.

Once realtime actually performs N -> N+1, however, the cutover is an ordered event and
must not be collapsed merely because N+2 becomes ready while background still lags.
Prepared cutovers therefore need ordered storage/transport sufficient for multiple
outstanding actual transitions.

## 14. Internal handoffs are not app-module event flows

The following are internal `GraphExecutor` actor interactions:

```text
RealtimeExecutor -> BackgroundExecutor
    produced block chains
    actual generation cutover

BackgroundExecutor -> RealtimeExecutor
    immutable persisted-state version

AsyncCapacityManager -> producers
    provisioned/recycled blocks

BackgroundExecutor -> AsyncCapacityManager
    released completed blocks
```

They should not be drawn as children/siblings in app-module event-propagation diagrams.

The application-level graph-change flow is simply:

```text
ProjectGraph -> GraphExecutor
```

`GraphExecutor` then performs the internal staging and actor coordination described
above.

## 15. Non-goals / stop conditions

Do not add any of the following without a new explicit design requirement:

- global atomic pinning across independent producer queues;
- a globally contended logical queue `size` used by producer and consumer;
- realtime dynamic allocation/resizing;
- consumer-driven direct insertion into realtime producer reserves;
- reinterpretation of old-generation data under a new generation;
- latest-generation collapsing of cutovers that actually occurred; or
- generic configurable recording modes beyond the fixed overwrite/no-write/void
  semantics.
