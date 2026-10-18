# Runtime Performance Roadmap

_Status: implementation checkpoint. This note records known runtime scaling work
without expanding the normative execution-model documents. Semantic contracts in the
linked design documents continue to govern implementation choices._

The actor split, independently pinned producer-queue prefixes, generation draining,
and ownership model are not presently the main scaling concern. Persisted data has now
moved from a globally linear page index to structurally shared per-output range trees.
The next cluster of costs comes from per-value work where the data model already
exposes region, chunk, ring, or contiguous spans.

The governing performance invariant is:

> Realtime work must not grow with retained recording duration, and publishing a
> successor must scale with changed records and changed output metadata rather than with
> the complete persisted history.

## Ordered implementation work

1. **In progress in the active circular-index/store migration:** replace the persisted
   snapshot representation as one coherent unit:
   - resolve stable and generation-local output identities off realtime into compact,
     non-recycled handles; snapshots and realtime/background data access carry only
     those handles, never semantic identity variants or strings;
   - index each store-local handle slot through an immutable path-copy radix directory,
     so output selection performs a bounded sequence of direct digits rather than an
     output-count-dependent comparison search;
   - give each output a persistent/path-copy balanced range index keyed in explicit
     signed `GlobalIndex` order;
   - structurally share unchanged output directories, range subtrees, and record payloads;
   - retain per-output `IndexCoverage` in its immutable path-copied interval tree so
     one write/erase does not copy every historical coverage run;
   - make lookup, mutation, successor publication, and retired-root destruction
     independent of total retained recording duration except for nodes actually changed
     or made unreachable. The output directory, record trees, region/chunk payloads,
     span playback, and persistent `IndexCoverage` metadata are implemented in the
     active working tree; compiler/test verification of the coherent migration remains
     before this item is complete.
2. Complete semantic transition planning. Cutover work must be bounded, not necessarily
   copy-free. Preserve inherited state by reading frozen predecessor storage directly
   where appropriate, or by using compact transition-only carries/materializations.
   Small bounded migration copies are valid; synchronous copying of whole histories or
   unrelated raw regions is not. Prepared generations now own the cold semantic match
   of surviving sample/event port-state identities, including exact inherited timeline
   intervals and growing/shrinking extents; remaining physical transition realizations
   and bounded transition expiry still need implementation from that plan. Each inherited
   state is now also classified as already represented, transferable into one
   successor steady-storage view, requiring transition-only storage, or
   unavailable from the predecessor; shared successor views cannot be selected
   for transfers of distinct semantic states. Transferable states now produce
   concrete sample/event descriptors containing predecessor storage and
   conversion facts, the private successor storage view, and the exact inherited
   interval. Activation now receives the exact cutover sample and executes
   direct sample-ring, compact-sample-carry, and event-ring transfers at that
   boundary. Materialized sample histories and converted/indexed event
   representations remain transition-only until their bounded reconstruction is
   implemented.

   **Implementation-friction review (2026-10-18; unresolved):** the required fact is
   bounded inherited semantic state, not necessarily two complete compiled
   realizations. A separate transition/steady pair remains the documented way to keep
   the final realization free of temporary storage and work, but it has not yet been
   justified against either one successor realization with an expiring prepared
   overlay or deliberately private successor state. The latter removes the second
   handoff but may impose permanent copies/storage; the overlay may impose dynamic
   binding or branch cost. Complete the allocation-free direct-cutover path and one
   representative rewiring/fan-in transition before selecting among them. Record the
   measured lifetime, compile-time, steady-state and realtime-cutover costs here; do
   not add multi-realization ownership merely because the current single-realization
   types cannot express the first alternative.

   The leading overlay candidate is a stable steady-state `NodeStorage` prefix plus a
   compile-time-bounded transition-only suffix, shared by separately emitted
   transition and steady root entrypoints. This removes the second node-state
   migration: expiry only changes the selected entrypoint. Before adopting it, add an
   explicit steady-prefix boundary for nested-node state spans and quantify the dead
   suffix capacity retained by the contiguous allocation after expiry. The boundary
   and transition-only region declaration now exist in `NodeLayout`, but GraphJit does
   not yet allocate or bind a suffix. The tail must also remain discoverable as a
   semantic source when a newer revision supersedes an active transition; it must not
   acquire an ordinary persistent raw-region identity.

   Direct project-rate ring/carry cutovers now have a cold preparation stage which
   validates descriptors and storage bounds and rejects unsupported inherited-state
   forms before publication. The prepared realtime transfer performs no allocation and
   exposes no recoverable error path. `NodeStorage` lifecycle commit can still fail, so
   this does not yet make the complete activation boundary non-failing.

   **Implementation friction:** predecessor reconciliation currently begins in the
   executor only after successor lowering and allocation, too late to determine a
   transition suffix or emit a transition root. The same layout also creates the
   background storage instance, which would duplicate an appended realtime-only tail.
   Move immutable predecessor semantic inventories into the compile request and add a
   steady-only allocation extent for background storage before connecting suffix
   lowering.
3. Complete realtime persisted playback and recording span operations. Persisted
   playback now clears only requested sample windows, traverses relevant sample
   regions, copies ring-wrap runs, and binary-searches clipped event chunks. Realtime
   recording still needs bulk ring capture; direct materialization indexing and cached
   materialization coverage offsets/frame counts also remain.
4. Make `BackgroundGeneration` own a sealed reusable runtime skeleton: reader slots,
   storage realization objects, binding adapters, node/operation frames, reusable replay
   buffers, and pre-indexed static routes. Transactions supply new root pins,
   coverage/selections, produced payload views, and logical reset state without
   reconstructing that topology. Stream queued block chains into bulk region construction
   and merge sorted event ranges rather than staging and sorting individual values.
5. Replace the capacity manager's periodic producer scan with low-watermark and returned-
   block edge notifications, and replace per-output work wakes with one wake for all
   chains finalized by a realtime callback.
6. Optimize compiler plan searches and other cold metadata construction only after the
   runtime scaling work above.

## Additional ownership constraints

- A stable persisted-output identity and a generation-local output identity have
  different retirement rules even when both resolve to compact store slots. Slots must
  not be recycled while any snapshot, generation, queued route, or transition can still
  name them.
- Immutable background topology and transaction-resettable state are separate. Reusing a
  runtime skeleton must not let failed or stale transaction state leak into the next
  transaction.
- Current realtime queue blocks return through generation-owned producer reserves. A
  persisted record cannot retain one of those blocks after that reserve dies. Zero-copy
  queue-block-to-record promotion therefore requires an explicit transfer to
  executor-level ownership (or equivalent record backing), not merely retaining the
  existing block.
- Retired-root reclamation must be serviced internally or guaranteed by the host, and
  opportunistic background reclamation must have a bounded budget so it cannot create an
  unbounded actor stall.

## Known costs covered by this roadmap

- linear persisted record and coverage lookup;
- linear record insertion, erasure, and mutation deduplication;
- full record-pointer copying, full metadata reconstruction, and history-sized reference
  release for every persisted snapshot successor;
- per-frame/per-channel persisted playback and per-value realtime capture;
- retained-event-ring rescans for every callback;
- linear Tick-materialization lookup and repeated coverage-prefix walks;
- per-value sample-region reconstruction and concatenate-plus-sort event-chunk rebuilding;
- synchronous unbounded graph-cutover copying;
- per-transaction reader registration and reconstruction of background realization and
  call-frame topology;
- per-publication persisted-state allocation and reader-list mutex traffic;
- repeated runtime discovery of static port/storage/route relationships;
- one-millisecond capacity polling, per-output background notification, and reclamation
  that currently depends on an explicit servicing command;
- lower-priority stack-pressure replanning and compiler metadata searches.

This is a static-review priority order, not benchmark evidence. Measurements should
validate constants and later optimization choices, but no measurement is needed to
justify removing work whose asymptotic cost grows with the complete recording on the
realtime or successor-publication path.
