# Module Ports before authorable inner connections

This passage came from [Module Interoperability And Graph Macros Direction](../module_interoperability_and_graph_macros_direction.md).
The [current Module Ports design](../module_interoperability_and_graph_macros_direction.md#module-ports-and-child-lanes)
replaced it on 2026-11-21.

> When a module with public ports expands, **Module Ports** is its first child
> lane. It gives project edits access to the same public ports as the module
> header. External cables terminate on the module header. Connections between
> the public ports and the module's children use the boundary lane. C++
> connections between shown nodes and to or from these boundary ports appear
> as solid gray cables. Editing those cables belongs to the module source.
> The boundary lane has no independent node or saved state.
> Its knobs edit the same manual input values and output gains as the module
> header.

## Earlier child lane styling

The same [design section](../module_interoperability_and_graph_macros_direction.md#module-ports-and-child-lanes)
described the earlier child lane styling. The owner chose inset shadows with
aligned edges and striped headers on 2026-11-21.

> C++ child lanes use a muted header and keep their knobs editable. Nested lanes align
> with the other lanes. A shadow marks the group, and a narrow guide shows its depth.
