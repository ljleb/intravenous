# Realtime / Background Execution, Queues, And Capacity

_Status: normative runtime direction for `GraphExecutor` execution ownership, realtime/background handoff, asynchronous capacity provisioning, and hot reload._

## 1. One app module, two internal execution actors

`GraphExecutor` is one application module.

Internally it owns separate execution actors and runtime infrastructure:

```text
GraphExecutor
|- RealtimeExecutor
|- BackgroundExecutor
|- AsyncCapacityManager
|- producer reserves / background pending queues
`- background -> realtime persisted-state mailbox
```

The actors are separate because they have different thread ownership, latency rules,
and mutable state. They are **not** separate app modules.

Application modules may have bidirectional static relationships. The separate rule in
[event_propagation_tree_constraint.md](./event_propagation_tree_constraint.md) is that
one dynamic app-module event propagation must form at most a tree. Internal
`RealtimeExecutor <-> BackgroundExecutor` handoffs are not app-module event
propagation and do not appear in those trees.

`RealtimeExecutor` and `BackgroundExecutor` share no mutable executor state. Large
cross-actor transfers use stable pointers/ownership transfer to already allocated
objects or blocks rather than copying payloads merely to cross the actor boundary.

## 2. `ExecutionGeneration` is the hot-reload unit

One successful `GraphJit` result defines one logical execution generation. Runtime
preparation builds one complete heap-owned `ExecutionGeneration` off to the side:

```text
CompiledGraph N+1
      |
      v
construct ExecutionGeneration N+1 completely
      |
      +-- realtime realization/state-migration plan
      +-- background realization/state-migration plan
      +-- generation-specific routes and producer endpoints
      +-- pre-sized background input/work descriptors
      +-- required producer-reserve capacity
      `-- storage needed to publish the generation cutover
      |
      v
publishable pending generation pointer
```

Conceptually:

```cpp
struct ExecutionGeneration {
    GenerationId id;
    RealtimeGeneration realtime;
    BackgroundGeneration background;

    // Generation-specific routes/endpoints/work descriptors.
    // May also carry intrusive linkage used after an actual cutover.
};
```

The exact C++ nesting is implementation detail. The invariant is not:
"prepare background, then prepare realtime". The invariant is stronger and simpler:

> An `ExecutionGeneration*` is publishable as pending only after both halves and every
> resource required by the realtime cutover are ready.

If preparation fails, the partially constructed generation is discarded off realtime.
No actor observes a half-prepared successor.

Anything whose cardinality is known from the compiled graph should be sized here rather
than grown while realtime or background execution is running. This includes producer
endpoints, background input descriptors, route tables, recording destinations,
persisted-output mappings, migration entries and node/event routing tables.

## 3. Realtime actor

`RealtimeExecutor` owns realtime-thread mutable state, including:

- the active and optional pending `ExecutionGeneration*` realtime realization;
- canonical realtime `NodeStorage` and graph-revision state migration;
- whole-pass transition/steady activation;
- the active immutable persisted-state/page version selected for the current pass;
- at most the newest pending compatible persisted-state pointer received from
  background;
- realtime invocation/materialization bindings; and
- producer endpoints for recording, Tick/persisted output and any other dynamically
  accumulating realtime-produced inputs to background execution.

An in-progress realtime pass never changes generation or persisted-state version.
Pending replacements become active only at legal pass boundaries.

Realtime execution must not allocate dynamically, wait for the background worker,
resize cross-thread storage, reclaim heap objects, or destroy superseded immutable
versions.

## 4. Background actor

`BackgroundExecutor` owns:

- its worker thread;
- the current background generation and ordered generation cutovers already performed
  by realtime;
- background-only mutable evaluation state and optional `TockState`;
- canonical persisted-page/recording state and immutable publication roots;
- one pending queue per producer as required by the compiled generation;
- pre-sized descriptors used to pin the current finite workload;
- background coverage/propagation workspaces and transaction-local candidates; and
- exact workload selection and commit.

Incoming data may wake the worker, but it never modifies work already selected for the
current background operation.

Successful publication of a producer chain advances a shared work revision and wakes
the worker. Publication of an actual generation cutover does the same. Control commands
use the same allocation-free wait edge but do not advance the work revision, so staging
or reclamation cannot manufacture a background evaluation and cannot hide a concurrent
producer publication.

The worker selects one precise finite workload, executes it, commits or rejects it,
then selects again from the latest pending state. If more data is already available it
may immediately start the next pass.

The background worker remains the sole executor and owner of the background generation
chain, its `NodeStorage`, selected queue prefixes and propagation workspace. There is no
caller-driven `evaluate_background(request)` production path and no retained "last
successful request". Queue consumption must never depend on a prior control call.

Every work source has its own final actor-owned transport and lifetime:

```text
realtime-produced persisted/recorded data -> producer pending queues
semantic mutations                         -> background mutation input
external Random Access demand              -> background demand input
advance materialization demand             -> background scheduling input
activated generations                      -> ordered generation chain
```

When woken, the worker independently pins a finite prefix from each relevant pending
queue and selects finite inputs from the other sources. Realtime-produced records
already identify their routed output and global timeline window; decoding them derives
their exact coverage and changed-coverage roots. No externally supplied evaluation
request is required to discover or process those records.

Transaction context is actor-owned. Stored-page width comes from the active compiled
generation's fixed root block size. The target semantic version comes from the
background actor's selected semantic environment. Mutation and demand roots come from
the actor's selected inputs. A private transaction-local aggregate may hold those facts,
but it is not a public execution command and it is never preserved for compatibility.

One transaction executes serially on the background worker. That synchronous
select/execute/commit operation is intentional; synchronously asking another thread to
start it and blocking the caller is not.

Implementation proceeds directly toward these ownership boundaries. Transitional
public drivers, compatibility wrappers, retained request objects, and other code whose
only purpose is to keep an intermediate revision compiling are forbidden. An
intermediate revision may instead be temporarily incomplete or fail to compile while a
self-contained architectural piece is being replaced; code that cannot support the
final execution model must be deleted rather than disguised as a temporary API.

Explicit reclamation of retired persisted-state roots, persisted pages and Tick
materializations is submitted to that same worker, keeping publication/retired-owner
mutation single-threaded. Reader-slot registration and unregistration may still occur
during off-realtime generation staging or destruction, so those registries synchronize
with background reclamation; realtime root pinning itself remains lock-free.

## 5. Producer reserve and pending queue are different roles

The runtime's logical SPSC queue is implemented from two ownership-facing pieces:

```text
AsyncCapacityManager
        |
        | assigns ready blocks
        v
ProducerReserve                 Background PendingQueue
[free] -> [free] ...              [work] -> [work] ...
        ^                                   ^
        |                                   |
   realtime takes                     background consumes
```

`ProducerReserve` answers only the realtime-side question:

> Is already allocated writable capacity available, and where is the next block?

`PendingQueue` answers only the background-side question:

> What published work remains, and what prefix has this background workload selected?

They need not be one public container object. Treating them separately keeps the
producer, consumer and capacity-manager synchronization domains small.

The backing blocks are stable, fixed-capacity, and power-of-two-sized. Queue users do
not require a logical `size()`, logical indices, or a contiguous resizable array.

## 6. Private chain construction and cheap publication

The producer owns a block exclusively before publication. It may initialize payload,
metadata, used count and private links with ordinary non-atomic writes.

Conceptually:

```text
producer-private

[A] -> [B] -> [C] -> null
```

The producer passes at least:

```cpp
struct ProducedBlockChain {
    Block* first;
    Block* last;
};
```

Only the handoff that makes the already initialized chain visible to background
requires publication synchronization. The background-side queue attachment is a cheap
pointer publication: install the first pointer when empty, or atomically connect the
old pending tail to `incoming.first`, then remember `incoming.last` as the new tail.

The precise atomic representation is implementation detail. The required guarantee is:
if background can observe the published head/link, every payload and private link in
the published chain is fully initialized and safe to read.

There is no per-entry publication protocol. Current audio/event production has a
natural pass/block publication boundary.

## 7. Direct production into provisioned blocks

Where layout permits, realtime audio/event output should be written directly into
blocks obtained from the producer reserve.

Preferred path:

```text
AsyncCapacityManager provisions block
            |
            v
ProducerReserve
            |
            v
RealtimeExecutor / generated DSP writes final payload directly
            |
            v
producer publishes completed chain
            |
            v
BackgroundExecutor PendingQueue
```

Do not create a second realtime recording/capture buffer merely to copy it into a queue
block afterward when the provisioned block can be the original destination.

Recording blocks may themselves represent the recording operation: destination,
absolute range, payload/void disposition, and payload storage. There is no requirement
for a separate "capture record" object layered over queue storage.

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
following newly appended links.

There is intentionally **no atomic snapshot relationship across different queues**.
If queue A and queue B are changing while background selects work, newly published
items may land in either the current or the next background pass independently.

This is an explicit application assumption, not a race to be "fixed".

If a future feature requires atomic visibility or ordering across producer queues,
implementation must stop and that requirement must be designed explicitly. Do not add
implicit global locking, a globally shared queue size, or cross-queue snapshot
synchronization to the generic queue mechanism.

If two pieces of information are semantically required to become visible atomically,
they should normally be one published object/chain or carry an explicit shared
version/identity.

## 9. Work selection uses pre-sized descriptors

The compiled generation already determines how many producer inputs background may
need to inspect. `ExecutionGeneration` therefore owns a fixed-size descriptor array (or
equivalent stable structure) prepared off-thread.

Work selection fills pointer fields such as:

```cpp
struct BackgroundInputSelection {
    PendingQueue* queue;
    Block* first;
    Block* last;
};
```

No map/vector growth is required merely to describe the selected workload.

Once the worker has filled the selected `(first,last)` pairs and selected any required
immutable version pointers, that workload is fixed. Later queue appends, desired-range
changes, node/background changes, compiled generations, or persisted-state publications
become inputs to later work.

## 10. `AsyncCapacityManager`

`AsyncCapacityManager` is runtime infrastructure owned by `GraphExecutor`, not an app
module. Its non-realtime worker provisions producer reserves and reclaims released
blocks.

The producer supplies the structural burst requirement and reserve policy. The useful
producer-facing values are:

```text
C = maximum producer burst that must fit without new provisioning
L = low ready-capacity watermark
H = refill target
```

`C` is a producer-derived structural bound used to validate/derive reserve policy.
`L` covers tolerated provisioner detection/scheduling delay plus margin. `H` provides
headroom and hysteresis. Normally `C << L < H`.

Allocation/slab granularity is an allocator implementation choice and need not be part
of every producer's semantic configuration. The capacity manager may round refills to
its own power-of-two slab/segment granularity.

When a producer's ready reserve falls below `L`, the manager provisions/recycles enough
whole blocks to move back toward `H`.

Provisioning and reclamation are independent. Slow background execution may retain old
blocks while the manager allocates additional reserve for the producer. There is no
fixed-duration ring whose fullness authorizes normal data loss.

The manager may keep its own allocation/release accounting and tail pointers. Those
are capacity-management state, not queue semantics exposed to producer or consumer.

## 11. One released-block stream is sufficient

The forward direction remains producer-specific because every producer needs an
independent reserve and pending queue.

The return direction does not need one reclamation queue per producer when one
`BackgroundExecutor` worker releases completed blocks and one `AsyncCapacityManager`
reclaims them. Completed blocks from all pending queues may enter one SPSC released
block stream:

```text
PendingQueue A --\
PendingQueue B ----> BackgroundExecutor -> released-block stream -> AsyncCapacityManager
PendingQueue C --/
```

Each block carries enough owner/type metadata for the manager to return/reassign it to
the appropriate reserve or allocator class.

This is an implementation simplification, not a requirement that heterogeneous
payload blocks share one layout.

## 12. Ownership cycle

Block ownership follows one direction:

```text
AsyncCapacityManager
        |
        | assigns ready capacity
        v
ProducerReserve
        |
        | producer acquires
        v
producer-private
        |
        | publish complete chain
        v
BackgroundExecutor PendingQueue / selected work
        |
        | successful release
        v
released-block stream
        |
        v
AsyncCapacityManager
```

At each point exactly one subsystem owns mutation rights appropriate to that state.

Background does not splice completed blocks directly into a realtime producer reserve.
Realtime never deletes/reallocates published blocks. Heap-owned objects referenced by
queue entries need equally explicit ownership transfer or immutable/shared lifetime
rules, and their reclamation must not fall onto the realtime thread.

## 13. Recording semantics on the queue transport

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

Where possible, the DSP writes the recorded sample/event payload directly into the
provisioned recording block. A `write_void()` block carries the addressed range but no
payload. An untouched block produces no queue entry at all.

## 14. Background -> realtime persisted state is a latest-version mailbox

Persisted-state/page publications are complete immutable versions, not accumulating
work items. Realtime normally needs only the newest compatible version available at a
pass boundary.

Therefore the background-to-realtime path is a mailbox, not a queue:

```text
BackgroundExecutor
      |
      | newest immutable PersistedStateVersion*
      v
RealtimeExecutor pending mailbox
```

If background publishes version B and then C before realtime consumes B, C may replace
B in the pending mailbox. Replacing a pending version never destroys its owner inline:
the superseded owner joins the same intrusive return stream used for replaced realtime
active versions and is destroyed only by explicit off-realtime reclamation.

At a legal realtime pass boundary, realtime takes the newest compatible pending
pointer and makes it active. The current pass never changes underneath execution.

The initial persisted-state owner for a staged successor is allocated and registers
its page reader during staging, but it does **not** pin the canonical page root then.
The cutover pins the newest canonical page root and completes the successor's empty
generation-local materialization without allocation, then installs that root directly
as realtime-active state before publishing the successor pointer to the background
actor, rather than publishing the root through the background mailbox.
Otherwise an old-generation background commit occurring after staging but before
cutover would be temporarily hidden by a stale successor root, and a racing late
old-generation mailbox publication could displace the successor's initial root. Queued
old-generation work that commits after the cutover still becomes visible through the
normal generation-aware background migration and mailbox publication path.

Generation compatibility is mandatory: a final old-generation version may be an input
to background migration, but it cannot become the active persisted view of a newer
realtime generation merely because it was published later in wall-clock time.

## 15. Generation hot reload

### 15.1 Preparation

`ProjectGraph` supplies generation N+1 once to `GraphExecutor`.

`GraphExecutor` completely constructs `ExecutionGeneration N+1`, including both actor
halves, generation-specific routes/endpoints, pre-sized work descriptors, migration
metadata, reserve requirements and any storage needed for allocation-free publication.
Only then can the pointer become the pending generation.

Preparing background-storage migration is itself submitted synchronously to the
background actor. The staging caller may own the not-yet-published successor object, but
it never inspects or prepares migration from actor-owned predecessor `NodeStorage`
concurrently with evaluation. Any preparation failure is therefore still reported and
the successor discarded before realtime can publish the cutover.

A staged generation is not active merely because preparation completed. A pending
successor that never activates may be superseded and reclaimed off realtime without
creating a semantic generation boundary.

### 15.2 Realtime-authoritative cutover

The actual generation transition happens at a legal realtime pass boundary.

The boundary performs, in order:

```text
1. publish all final generation-N producer chains from the completed pass
2. publish the already-prepared ExecutionGeneration N+1 pointer as the next cutover
3. swap RealtimeExecutor active generation N -> N+1
4. advance the background work revision and wake the background actor
```

These are bounded pointer/state operations and require no dynamic allocation.
Publishing the generation pointer does **not** synchronously run background evaluation.

No separate cutover allocation is required when the prepared `ExecutionGeneration` itself carries the information/linkage background needs to observe the transition.

### 15.3 Ordered actual cutovers

Once realtime actually performs N -> N+1, that transition is semantic history and must
not be collapsed even if N+2 becomes ready while background lags.

Actual successor generation objects may therefore form an intrusive ordered chain (or
an equivalent preallocated ordered transport):

```text
N+1 -> N+2 -> N+3
```

No cutover-node allocation occurs at the realtime boundary.

### 15.4 Old generation queues close

After N -> N+1, no generation-N producer endpoint can publish again. Every old pending
queue therefore has a finite tail.

The background worker may still be finishing already selected N work, draining remaining
closed N queues, or preparing the N -> N+1 retained-state migration. All such data is
interpreted using generation N.

Only after required N work is complete does background transition to N+1 and interpret
N+1 queue data under the new generation.

This requires no global atomic snapshot across old queues. Queue closure is sufficient;
background may drain each finite queue independently.

### 15.5 Removed producers and destinations

Stable logical identity, never generation-local slot/index reuse, determines continuity.

If producer and destination both survive, old-generation writes are drained under N,
retained destination state migrates, and N+1 writes continue normally.

If the producer disappears but the destination survives, remaining N writes are drained
and the destination's final state migrates. No N+1 writes arrive from the removed
producer. For recording, absence of later writes preserves retained content.

If the destination disappears, remaining N writes are still valid N work. The
destination is retired during migration and N+1 has no route to it.

If both disappear, remaining N work may finish and both identities are then absent from
N+1.

Replacement/install order must not change these outcomes.

## 16. Internal handoffs are not app-module event flows

The following are internal `GraphExecutor` interactions:

```text
RealtimeExecutor -> BackgroundExecutor
    produced block chains
    actual ExecutionGeneration* cutovers

BackgroundExecutor -> RealtimeExecutor
    newest immutable PersistedStateVersion* mailbox publication

AsyncCapacityManager -> ProducerReserve
    provisioned/recycled blocks

BackgroundExecutor -> AsyncCapacityManager
    one released-block stream
```

They should not be drawn as children/siblings in app-module event-propagation diagrams.

The application-level graph-change flow is simply:

```text
ProjectGraph -> GraphExecutor
```

`GraphExecutor` performs internal generation preparation and actor coordination.

## 17. Non-goals / stop conditions

Do not add any of the following without a new explicit design requirement:

- global atomic pinning across independent producer queues;
- a globally contended logical queue `size` used by producer and consumer;
- realtime dynamic allocation/resizing/reclamation;
- consumer-driven direct insertion into realtime producer reserves;
- reinterpretation of old-generation data under a new generation;
- collapsing cutovers that actually occurred to a latest-generation pointer;
- preserving every intermediate background persisted-state publication when only the
  newest compatible version matters; or
- generic configurable recording modes beyond the fixed overwrite/no-write/void
  semantics.
