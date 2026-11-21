# Module child addressing before the selector clarification

On 2026-11-20, the module interoperability document used virtual-node backing
to define which children a project could address.

> Project addresses for C++-authored children require backing by a virtual node.
> Hide children without virtual backing when the module is expanded.
> Project-authored connections cannot target these children. A future inspection
> or module-copy operation can revisit their visibility.

The owner clarified that the selector syntax defines addressability. The
[current rule](../module_interoperability_and_graph_macros_direction.md#nested-module-presentation-and-project-owned-scopes)
also includes children reached by a named tile selector.
