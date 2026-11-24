# Former nested node declarations

_Historical, non-normative. Preserved on 2026-11-24 when the owner removed
node declarations inside another node's `DeclarationContext`._

`do_declare()` previously accepted a parent `DeclarationContext`.
`nested_node_states()` bound a parent `State` member of type
`std::span<std::span<std::byte>>` to the state of its directly declared
children. The children acquired their own lifecycle records while execution
scheduled the parent as the graph primitive.

[GraphJit direction](../graph_jit_direction.md) formerly included:

> 21. **Finish remaining authored-node semantics** (nested declarations, compiler-
>     owned regions, activity/TTL, detach, events and skip scheduling).

[Front 7](../implementation_fronts_direction.md#front-7-remaining-graphjit-and-node-execution-semantics)
previously listed nested declarations among the remaining node semantics. The
[performance roadmap](../runtime_performance_roadmap.md) recorded the original
implementation:

> **Front 7 declaration-layout follow-up implemented in source (2026-10-21; human
> verification pending):** nested declarations and authored local/shared arrays now
> use the existing packed `NodeLayout` storage path. A top-level primitive map keeps
> nested records separate from graph primitives. Lowering resolves the map and emits
> final state offsets. No callback performs a map lookup. Shared-array dependency
> ordering and span patching remain cold initialization work.

The [current declaration layout](../graph_jit_direction.md#lowering-boundary)
uses one layout record per primitive. A node can keep its own typed `State` and
local arrays. Separately scheduled nodes are configured through `GraphBuilder`.
