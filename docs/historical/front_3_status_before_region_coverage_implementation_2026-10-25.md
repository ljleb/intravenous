# Front 3 Status Before Region/Coverage Implementation

_Status: historical. This note preserves implementation-status text superseded on
2026-10-25 by the active Front 3 source and the status recorded in
[Implementation Fronts Direction](../implementation_fronts_direction.md#front-3-source-status-2026-10-25)._

Before Front 3 implementation, `global_index_migration_status.md` stated that the
future node-facing `SampleRegion` and `SampleCoverage` port algebra remained separate
from the store-internal `PersistedSampleRegion` representation. It also stated that
the persisted store deliberately did not introduce that future API early.

The active implementation uses the final registered-continuous-value names instead:
`ContinuousRegion<Value>`, `WritableContinuousRegion<Value>`,
`ContinuousCoverage<Value>`, and `WritableContinuousCoverage<Value>`. It keeps
`PersistedSampleRegion` as an owning runtime record and binds its storage through
`ContinuousRegionStorageView` at the callback boundary.

Before implementation, `implementation_fronts_direction.md` also recorded that the
runtime flattened Region decomposition and multiplicity through
`BackgroundPortPlan::random_access_input`,
`BackgroundPortPlan::authored_tock_output`, positional `IndexCoverage`, and scalar
sample reads and writes. That observation was the entry friction. The replacement
source carries logical Region lists through propagation, storage selection,
background evaluation, persisted publication, and Tick materialization.
