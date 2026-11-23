# Rejected Lane Label Annotation

This draft appeared in [Module Interoperability And Graph Macros](../module_interoperability_and_graph_macros_direction.md).
The owner rejected it on 2026-11-21 and clarified that the request concerned the
third argument of `IV_NODE`. See [Node Definitions And Instances](../node_definitions_and_instances_direction.md#future-package-identity-publication-and-type-construction-2026-11-15-clarification)
for the display name contract.

> C++ authors can override the secondary label of a local node declaration
> with `IV_LANE_LABEL("…")`. For example, `IV_LANE_LABEL("*") auto gain = signal * level;`
> shows `*` on the virtual lane and `* 1` on its first member. The selector still
> uses the declaration identity of `gain`. An empty label uses the declaration name.
