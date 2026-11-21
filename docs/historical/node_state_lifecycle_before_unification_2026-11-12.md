# Node State And Lifecycle Direction Before Unification (2026-11-12)

_Status: non-normative preservation of normative text superseded by the 2026-11-12
Tick/Tock authored-state and lifecycle unification decision. This archive records
the precise removed or replaced source text (with original file and line ranges)
from the repository snapshot `intravenous(20261112-122249).zip` (commit `ae173260`).
Original text is included for provenance only; the normative destination is in
[`Node Interaction And Presentation Communication Direction`](../node_interaction_and_presentation_communication_direction.md),
[`Graph JIT Direction`](../graph_jit_direction.md), and
[`Coverage And Background Evaluation`](../coverage_and_background_evaluation.md).

## Decision summary

- The former authored `Node::TockState` and separate
  `TockInitializationContext<Node>`/`TockReleaseContext<Node>` are superseded by
  one optional `Node::State` and one named lifecycle context family, with
  execution-domain-specific capabilities.
- The former fresh-only Tock state lifecycle becomes eligible for optional
  background-safe `move()` migration. Fresh initialization remains the safe
  fallback; state remains non-semantic acceleration.
- Tick and Tock storage stay separate. This does not authorize Tock access to
  packed realtime `NodeStorage`.

## Preserved superseded passages

The excerpts below reproduce changed portions of the previous documents. A
`text` fence preserves the original characters and wording; the source and
original line ranges identify their provenance. Unchanged implementation-status
records remain in their respective documents, explicitly marked as checkpoints.

## `docs/node_interaction_and_presentation_communication_direction.md`

### Former lines 8–8

```text
see [the preserved earlier design](./historical/node_presentation_and_live_observation_before_vue_direction_2026-11-12.md)._
```
### Former lines 57–57

```text
- validating the restrictions on explicit nested `State` and `TockState` types;
```
### Former lines 83–86

```text
`declare()` describes the packed realtime realization and therefore declarations
associated with `State` and compiler-owned audio-thread storage. `TockState` is not
part of this declaration/layout model and should not need a synthetic
`DeclarationContext::tock_state()` either.
```
### Former lines 88–88

```text
## `State`, `TockState`, and `NodeStorage` have different ownership
```
### Former lines 90–94

```text
`NodeStorage` exists to make the audio-thread realization as cheap to execute as
possible. It is the fixed-layout packed storage addressed by the JIT: authored
`State` plus compiler-selected audio-thread data that must survive execution calls.
Its layout should be chosen for compactness/locality and, where useful, roughly the
order in which generated audio-thread code will touch it.
```
### Former lines 96–101

```text
`TockState` is different. It is background-thread state used by Tock/background
work and must not be packed into `NodeStorage`. Ordinary dynamic allocation is
allowed inside `TockState`; its size and internal allocations may change as
background work changes. This is particularly important for acceleration structures
whose useful size depends on actual input coverage/content rather than on the
compiled realtime realization.
```
### Former lines 103–108

```text
A node may still set up its instance's `TockState` from `initialize()`. For example,
`initialize()` may allocate configuration/resource-dependent tables or establish
background acceleration state that is meaningful before any particular input
contents are examined. Input-dependent derived contents, however, must be computed
or recomputed by the appropriate background `tock*()` work rather than by
`initialize()`.
```
### Former lines 110–114

```text
`TockState` remains non-semantic acceleration/memoization state: observable results
must not depend on its history, and correctness must survive discarding and freshly
reinitializing it. Whether an implementation opportunistically retains/moves a
compatible `TockState` across realization changes is a background-lifecycle policy,
not part of `NodeStorage` compatibility.
```
### Former lines 116–116

```text
## Configuration, realtime state, background state, layout, and executable behavior are distinct
```
### Former lines 118–118

```text
A semantic node has at least five independent aspects:
```
### Former lines 121–124

```text
2. the contents of its realtime `State`;
3. its background-only `TockState`;
4. the realtime `NodeLayout` produced by `declare()`;
5. its compiled Tick/Tock behavior.
```
### Former lines 142–142

```text
`MyNode{8}` and `MyNode{32}` have the same `State` type but different realtime
```
### Former lines 144–144

```text
without altering the resulting `NodeLayout`. Changes to `TockState` allocation do
```
### Former lines 160–161

```text
and persistent-state semantics are compatible. Background `TockState` lifecycle is
managed separately.
```
### Former lines 176–176

```text
use dynamic storage in `TockState` and resize during `tock*()` work without creating
```
### Former lines 181–183

```text
> Configuration defines the realtime realization envelope; `State` represents the
> current realtime point within that envelope; `TockState` is reconstructible
> background acceleration state outside the packed realtime realization.
```
### Former lines 323–323

```text
A backend node cannot directly mutate another backend node's `State`, `TockState`,
```
### Former lines 504–504

```text
`State` and `TockState` retain their existing meanings as node execution state,
```
### Former lines 533–533

```text
`State`/`TockState` fields and port data. Exact observation hook syntax is deferred;
```
### Former lines 648–649

```text
Use existing `NodeStateStructure`/`NodeStateFieldStructure` reflection for `State`
(and the appropriate source metadata for `TockState`) together with resolved port
```
### Former lines 711–711

```text
`TockState` also have different thread ownership. Non-realtime code must not read
```

## `docs/graph_jit_direction.md`

### Former lines 1538–1538

```text
persisted-output data, and `TockState` are not part of this packed realtime arena.
```
### Former lines 1548–1548

```text
### `TockState` is background-owned sidecar state
```
### Former lines 1550–1554

```text
`TockState` is Tock/background acceleration state, not realtime node storage. It is
managed separately from `NodeStorage` and is never addressed by the audio-thread
root. Ordinary dynamic allocation is allowed inside it. Its internal size may
therefore follow actual background requirements such as input coverage/content
without changing `NodeLayout` or forcing a realtime-storage migration.
```
### Former lines 1556–1561

```text
A node may use `initialize()` to set up its instance's `TockState` from configuration
and resources. Any derived data that depends on current input contents/coverage is
computed or recomputed by the appropriate background `tock*()` work instead.
`TockState` remains non-semantic acceleration state: outputs must be correct from a
freshly initialized instance, and retaining/moving compatible acceleration state
across realization changes is only an optimization.
```
### Former lines 1563–1566

```text
Source introspection may still publish nominal/definition metadata for `TockState`
for lifecycle/hot-reload validation, but `TockState` field layout is not a
`NodeLayout`/`NodeStorage` placement contract. `State` remains the authored typed
state whose realtime storage relationship is fixed by declaration/layout.
```
### Former lines 1578–1578

```text
Examples include `TockState`, reusable background-evaluation workspaces, transaction
```
### Former lines 1602–1602

```text
conflating storage locality with lifecycle ordering. `TockState` placement is not
```
### Former lines 2202–2202

```text
frame. Background-owned state (`TockState`, evaluation workspaces, persisted data,
```

## `docs/coverage_and_background_evaluation.md`

### Former lines 121–121

```text
> `TockState` remains non-semantic acceleration available only to tock.
```
### Former lines 541–541

```text
`TockState` is **not** visible here. Forward dependency semantics must not vary
```
### Former lines 618–618

```text
`TockState` is **not** visible here. Dependency requirements may not depend on
```
### Former lines 657–658

```text
- cannot depend on sequential `State`;
- may use `TockState` solely as non-semantic acceleration/memoization state; and
```
### Former lines 660–660

```text
  contents/history of `TockState`.
```
### Former lines 662–662

```text
`TockState` has one deliberately narrow meaning:
```
### Former lines 668–672

```text
`TockState` is background-owned and is not part of packed realtime `NodeStorage`.
It may use ordinary dynamic allocation. A node may set up configuration/resource-
dependent acceleration data in `initialize()`, but anything derived from current
input contents or coverage belongs in the appropriate background `tock*()` work and
may be resized/recomputed there.
```
### Former lines 674–681

```text
A correct node must therefore produce the same result from a freshly initialized
`TockState`. The executor is free to serialize access, duplicate state per worker,
discard/reinitialize it, or otherwise manage acceleration state without changing
semantics. One mutable `TockState` instance must not be concurrently mutated by
overlapping tock executions unless the node's own state representation makes that
safe. All tock execution is background-only. `BackgroundExecutor` may serialize,
duplicate, retain, move, or reset compatible acceleration state without introducing
any audio-thread lock, live callback, or ephemeral-output exception.
```
### Former lines 1114–1116

```text
`TockState` is deliberately outside realtime `NodeStorage`. `BackgroundExecutor`
owns it as per-node background acceleration state, and it may contain dynamically
allocated structures. Reusable background workspaces are likewise background
```
### Former lines 1119–1121

```text
`TockState` is not an authoritative persisted-output store and is not shared with
`tick_block()` or propagation callbacks. A node's observable behavior must remain
correct if its `TockState` is discarded or independently instantiated.
```
### Former lines 1353–1353

```text
1. ordinary node configuration/resources and optional background `TockState` are
```
### Former lines 2256–2257

```text
at **every** construction path (including internal builder paths), malformed
`TockState`, conflicting callback/production declarations, invalid event
```
### Former lines 2266–2266

```text
The checked-in replayability validation accepts `tick()`, `tick_block()`, or
```
### Former lines 2268–2268

```text
`State`, `TockState`, Random Access inputs, nonzero port history or latency, and
```
### Former lines 2684–2684

```text
11. `TockState` is non-semantic Tock-only acceleration; canonical persisted-data
```

## `docs/batched_node_callbacks_direction.md`

### Former lines 232–232

```text
`output<Name>()`, and `tock_state()` with the same meaning as
```
### Former lines 236–236

```text
`TockState`. Coverage equality is not a batch-compatibility requirement.
```
### Former lines 386–388

```text
Configured values, `State`, `TockState`, port addresses, and requested
`IndexCoverage` need
not be equal across lanes.
```

## `docs/random_access_port_data_direction.md`

### Former lines 1002–1003

```text
If a node needs one background object per Region, its separately owned `TockState`
can dynamically resize during the appropriate `tock*()` callback:
```
### Former lines 1008–1008

```text
    -> TockState resizes/rebuilds acceleration data
```
### Former lines 1116–1116

```text
- `TockState` remains separately owned background state and may allocate dynamically;
```

## `docs/dsp_execution_storage_glossary.md`

### Former lines 476–478

```text
It is not the owner of `TockState`, background workspaces, dynamically sized
persisted-output data, or recording capture backlogs. `TockState` is separately
owned background acceleration state and may use dynamic allocation.
```

## `docs/project_graph_application_architecture.md`

### Former lines 543–543

```text
the resulting canonical `NodeLayout` covers realtime `State` plus
```
### Former lines 545–545

```text
no parallel graph-kernel realtime-storage arena. Background-only `TockState` and
```
### Former lines 679–679

```text
- separately owned lifecycle/storage for optional background-only `TockState`;
```

## `docs/realtime_background_execution_and_queues.md`

### Former lines 110–110

```text
- background-only mutable evaluation state and optional `TockState`;
```
### Former lines 566–566

```text
N+1 queue data under the new generation.
```

## `docs/sequential_port_storage_planning.md`

### Former lines 264–264

```text
activity state. Background-only `TockState` does not: it is separately owned by the
```
