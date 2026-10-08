# Runtime Performance Roadmap

_Status: implementation checkpoint. This note records known runtime scaling work
without expanding the normative execution-model documents. Semantic contracts in the
linked design documents continue to govern implementation choices._

The actor split, independently pinned producer-queue prefixes, generation draining,
and ownership model are not presently the main scaling concern. The largest defect is
that immutable persisted payloads sit behind a globally linear, non-persistent index.
The next cluster of costs comes from per-value work where the data model already exposes
page, ring, or contiguous spans.

The governing performance invariant is:

> Realtime work must not grow with retained recording duration, and publishing a
> successor must scale with changed pages and changed output metadata rather than with
> the complete persisted history.

## Ordered implementation work

1. Replace the persisted snapshot representation as one coherent unit:
   - resolve stable and generation-local output identities off realtime into compact,
     non-recycled handles; snapshots and realtime/background page access carry only
     those handles, never semantic identity variants or strings;
   - index each store-local handle slot through an immutable path-copy radix directory,
     so output selection performs a bounded sequence of direct digits rather than an
     output-count-dependent comparison search;
   - give each output a persistent/path-copy page index keyed by page position;
   - structurally share unchanged output directories, page subtrees, and page payloads;
   - update coverage/layout/type metadata from the changed leaves instead of rebuilding
     metadata from every retained page;
   - make lookup, mutation, successor publication, and retired-root destruction
     independent of total retained recording duration except for nodes actually changed
     or made unreachable.
2. Complete semantic transition planning. Cutover work must be bounded, not necessarily
   copy-free. Preserve inherited state by reading frozen predecessor storage directly
   where appropriate, or by using compact transition-only carries/materializations.
   Small bounded migration copies are valid; synchronous copying of whole histories or
   unrelated raw regions is not. Prepared generations now own the cold semantic match
   of surviving sample/event port-state identities, including exact inherited timeline
   intervals and growing/shrinking extents; physical transition realization and bounded
   cutover execution remain to be implemented from that plan. Each inherited
   state is now also classified as already represented, transferable into one
   successor steady-storage view, requiring transition-only storage, or
   unavailable from the predecessor; shared successor views cannot be selected
   for transfers of distinct semantic states.
3. Convert realtime persisted playback and recording to page/span operations. This
   includes clearing only requested sample windows, one lookup per relevant page/span,
   ring-wrap copy runs, retained event-window cursors, direct materialization indexing,
   and cached materialization coverage offsets/frame counts.
4. Make `BackgroundGeneration` own a sealed reusable runtime skeleton: reader slots,
   storage realization objects, binding adapters, node/operation frames, reusable replay
   buffers, and pre-indexed static routes. Transactions supply new root pins,
   coverage/selections, produced payload views, and logical reset state without
   reconstructing that topology. Stream queued block chains into bulk page construction
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
  persisted page cannot retain one of those blocks after that reserve dies. Zero-copy
  queue-block-to-page promotion therefore requires an explicit transfer to executor-level
  ownership (or equivalent page backing), not merely retaining the existing block.
- Retired-root reclamation must be serviced internally or guaranteed by the host, and
  opportunistic background reclamation must have a bounded budget so it cannot create an
  unbounded actor stall.

## Known costs covered by this roadmap

- linear persisted page and coverage lookup;
- linear page insertion, erasure, and mutation deduplication;
- full page-pointer copying, full metadata reconstruction, and history-sized reference
  release for every persisted snapshot successor;
- per-frame/per-channel persisted playback and per-value realtime capture;
- retained-event-ring rescans for every callback;
- linear Tick-materialization lookup and repeated coverage-prefix walks;
- per-value sample page reconstruction and concatenate-plus-sort event page rebuilding;
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
