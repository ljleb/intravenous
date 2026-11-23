# Automation before private leaves

Archived on 2026-11-19 from the [builtin automation guide](../../src/intravenous/builtin_packages/builtin/AUTOMATION.md).
The owner made the point source and interpreter private and kept the editable
Automation Curve as the public module.

> The builtin package provides `builtin.automation.curve`, an editable module.
> Its leaves are `builtin.automation.point_source` and
> `builtin.automation.interpreter`. Both leaves execute as Tock nodes. The
> point source publishes buffered event data. The interpreter computes Sample
> Coverage data only for requested windows.

The guide previously described creating an explicit empty point list in Vue.
The picker now uses the module's default constructor, which starts with zero
points. The earlier example also showed forwarding a point event output. The
current Automation Curve exposes its sampled `value` output.
