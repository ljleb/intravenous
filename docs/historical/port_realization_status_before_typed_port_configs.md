# Port-realization status before typed authored port configs

_Historical implementation record preserved on 2026-10-14 from
`docs/port_realization_migration_status.md` before that status document was updated
for the typed authored/configured port-config feature. The current implementation
record is [Port Realization Migration Status](../port_realization_migration_status.md)._

## Superseded expected repository state

The repository is **not expected to compile** at this milestone. Expected compiler
errors belong to this unfinished feature and are concentrated in tests that still use
removed config timing fields or the old connection-analysis signature. No compatibility
wrapper should be introduced to make those call sites compile; migrate them after the
pace-aware execution shape is complete enough to define their final assertions.

## Superseded next bottom-up connection

Carry projected per-port extents into generated Tick bindings and storage planning.
Then migrate the remaining tests, add execution-path cycle/range/dimension regression
coverage, and refactor the solver around the stable responsibilities discovered by the
working execution path.

These paragraphs described an earlier incomplete migration boundary. They are retained
here as implementation history, not as the normative destination.

The earlier incomplete-milestone list also stated:

> The remaining unit tests that directly construct timing-bearing legacy configs and
> directly invoke connection analysis have not yet been migrated to authored constraints
> and explicit resolved-realization input.

That migration subsequently completed.

## Discarded intermediate configured representation

During the typed-port implementation, an intermediate working-tree representation
stored a `ContinuousValueTypeSet` beside the vector of type-owned metadata alternatives.
It also projected Sample neutral/default/range values into parallel fields on the
configured Sequential input. This was removed before completing the feature: the
metadata alternatives already identify the exact value-type domain and already contain
the Sample values, so both projections admitted disagreement without adding semantic
information.

The replacement derives the configured value-type domain from unique metadata
alternatives. Authored declarations reject repeated value types statically, and archive
validation rejects empty or repeated metadata domains. The current implementation
record documents that replacement.

A subsequent intermediate draft attempted to retain channel layout both on the
configured port and inside each audio-capable metadata alternative, with a helper that
rewrote all copies together. Source tracing through tiled and virtual ports showed that
this would turn a generic port fact into permanent cross-layer synchronization work.
The completed representation instead uses extension-owned authored options to establish
one common channel layout, promotes it to the configured port, and omits it from erased
type metadata.
