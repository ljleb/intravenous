# Superseded Separate Dyadic Execution Index

_Status: historical, non-normative._

_Provenance: this direction was introduced during the port-pace migration on
2026-10-14 and superseded the same day after comparing the execution address with the
already-planned transported fractional global coordinate._

_Reason for supersession: `DyadicGlobalIndex` and `FractionalGlobalIndex` had the same
Q64.64 circular domain and required the same arithmetic. Their only distinction was
the contextual role of a value—current execution address versus coordinate transported
as graph data—which does not warrant incompatible C++ types. Keeping both duplicated
range algebra and forced semantically empty conversions in resampling and Random Access
nodes._

## Preserved direction

Generated execution was briefly specified to use a separate `DyadicGlobalIndex`, made
from a wrapping `GlobalIndex` whole part and 64 fractional bits. Its associated
`DyadicGlobalExtent`, `DyadicGlobalIndexRange`, and `DyadicLatticeProjection` projected
one shared execution interval onto each power-of-two port lattice. The transported
`FractionalGlobalIndex` used the same representation but was treated as a distinct graph
value type.

The replacement uses `FractionalGlobalIndex` and its range/extent family for both
execution/storage addressing and transported coordinate values. Port pace still selects
a dyadic lattice within that common coordinate domain.
