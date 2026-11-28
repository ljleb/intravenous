# Front 3 `IndexCoverage` Output Semantics Before Region-List Publication

_Status: historical. This note preserves normative text superseded on 2026-10-25 by
the pre-allocated Region-list output contract in
[Random-Access Port Data Direction](../random_access_port_data_direction.md#coverage-outputs-receive-pre-allocated-regions)
and the corresponding implementation decision in
[Implementation Fronts Direction](../implementation_fronts_direction.md#front-3-region-output-allocation-decision-2026-10-25)._

Before the Region-list decision, `coverage_and_background_evaluation.md` described a
computed tock output as directly publishing one exact `IndexCoverage`. Its forward
update was described as publishing:

1. the output's new exact `IndexCoverage`; and
2. a sound region superset inside the old/new coverage containing every value that
   may have changed.

It also stated that output `IndexCoverage` was semantic and exact, and that no general
fallback could invent it.

That description was sufficient while output existence was modeled only as a set of
positions. It cannot preserve overlapping Regions, duplicate Regions, Region order,
or Region count. The replacement contract makes the logical Region-range list the
semantic output shape. The runtime derives an exact positional `IndexCoverage` union
from that list. `IndexCoverage` remains the positional algebra for propagation,
demand, invalidation, and storage selection, but it is not the data-bearing Coverage
or its publication format.
