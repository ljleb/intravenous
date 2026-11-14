# Front 3 sparse recording before the interleaved-only decision

_Provenance: archived from `docs/implementation_fronts_direction.md` on
2026-11-01 when the remaining continuous-layout question was resolved. The
replacement is the active Front 3 source status in that document._

The preliminary sparse-page direction did not yet select a representation for
multichannel planar Tick recording. `ContinuousRegionStorageView` has one storage
pointer. A growing planar Region changes the dense start of every channel after
channel zero. Separate virtual mappings could keep each channel contiguous, but
they could not preserve the dense whole-layout `elements()` span without changing
the public Region ABI or copying the full Region.

The open alternatives were a new per-channel base/stride ABI, repacking on growth,
or a forced representation conversion. This question blocked implementation of the
sparse recording store. Authored Tock Region/Coverage output was not part of the
question because it publishes complete independent Regions rather than appending a
live recording.

