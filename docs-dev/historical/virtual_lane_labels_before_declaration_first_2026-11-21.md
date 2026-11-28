# Earlier Virtual Lane Label Order

This wording appeared in [Module Interoperability And Graph Macros](../module_interoperability_and_graph_macros_direction.md).
On 2026-11-21 the owner chose to put the C++ declaration name first on virtual
lanes. The linked document describes the revised order.

> Each virtual node has a lane with the node type as its primary label and
> the C++ declaration name as its secondary label. Expanding the lane shows
> its concrete members. A member lane uses its own type and adds a one-based
> index to the declaration name. When a tile expands, its children show
> their channel names as secondary labels.

The first revision changed the order for virtual lanes only. Concrete member
lanes still put their type first and their declaration name and index second.
Tiled children still had their channel names second. The owner then requested
the same order for concrete lanes.
