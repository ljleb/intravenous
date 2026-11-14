# Front 3 segmented recording Region proposal before sparse pages

_Status: historical, non-normative._

_Provenance: preserved from `docs/implementation_fronts_direction.md` on
2026-11-01. The text recorded the first proposed response to the conflict between
canonical recording coverage and capture-block storage. It was superseded because a
segmented public Region would violate the accepted contiguous Region contract. The
current proposed replacement is in
[Front 3 source status](../implementation_fronts_direction.md#front-3-source-status-2026-10-26)._

## Preserved proposal

This recording rule exposes a storage/API conflict that must be settled before Front 3
can close. `PersistedDataStore::Candidate::overwrite()` path-copies independent
`PersistedSampleRegion` records so publishing one new capture block does not copy the
complete recording. For example, captures `[0, 4)` and `[4, 8)` remain two physical
records while their `IndexCoverage` is the one logical Region `[0, 8)`.
`ContinuousRegionStorageView`, however, contains one `storage` pointer and requires one
contiguous allocation for the complete logical Region. It cannot describe that
recording without either copying `[0, 8)` or gaining a segmented readable
representation. Copying the whole merged Region during every successor publication
would violate the persisted-store scaling invariant. Materializing it for each reader
would repeat request-sized work and cannot serve a Tick-time Random Access callback
without realtime allocation. Retaining the two capture blocks as two public Regions
would violate the canonical `IndexCoverage` rule. The leading correction is therefore
a segmented readable continuous Region whose logical range remains `[0, 8)` while its
storage can reference both retained records; its public bulk-access contract must be
settled before implementation because `ContinuousRegion<Value>::elements()` and
`channel_elements()` currently return one contiguous `std::span`.

## Reason for supersession

A Region must remain contiguous. Segmenting `ContinuousRegion<Value>` would move a
physical storage compromise into the public node interface and would prevent existing
algorithms from receiving a pointer and an extent for one Region. The replacement
proposal keeps the Region contiguous in virtual address space. It moves sparse physical
storage into `PersistedDataStore`, which already owns retained recording data.
