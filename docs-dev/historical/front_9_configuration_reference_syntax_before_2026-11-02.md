# Front 9 Configuration Reference Syntax Before 2026-11-02

_Status: historical. This document preserves design text that the Front 9 decision on
2026-11-02 superseded. The current design is in
[Configuration Expression Language Direction](../configuration_expression_language_direction.md)._

Before the decision, the design left the reference syntax open. Several documents used
`ref(user_authored_node_id)` and `select(virtual_node_name)[idx]...` as candidate forms.
The decision replaced these forms with a stable project-node ID literal and three
postfix selectors.

The following text came from `node_definitions_and_instances_direction.md`:

> The accepted language is intentionally much smaller than C++. It should support typed
> literals, nested braced initialization/registered aggregates and containers, ordinary
> numeric arithmetic, an allow-listed set of pure mathematical functions, and a small
> framework query/reference vocabulary. Candidate framework forms include
> `ref(user_authored_node_id)` and `select(virtual_node_name)[idx]...`; exact grammar and
> spelling remain implementation details of that front.

The following text came from `project_file_serialization_direction.md`:

> The persisted language is deliberately not arbitrary C++. It may include numeric
> operators, an allow-listed set of pure mathematical functions, and framework reference
> forms such as `ref(...)` / `select(...)[...]`.

The following text came from `project_graph_application_architecture.md`:

> This language is intentionally not general C++. It may provide scalar/string/enum
> literals, numeric arithmetic, a small allow-listed set of pure `cmath`-like functions,
> and framework-provided reference/query forms such as
> `ref(user_authored_node_id)` or `select(virtual_node_name)[idx]...`.

The matcher direction also required a separate structural subgraph selector. It allowed
virtual-node selection to search all descendants under the current cursor. The
following text came from `graph_builder_embedding_and_matchers.md`:

> The current path vocabulary has separate selectors for virtual node identity, ordered
> virtual members, tiled children, and nested subgraph scopes.

> The node path is an ordered sequence of structural selectors. It must be able to
> express arbitrary alternation between virtual-node selection, ordered direct-member
> selection, tiled-child/channel-member selection, and descent through nested subgraph
> scopes.

> Paths can navigate virtual -> member -> tile -> child scope -> virtual repeatedly.

The 2026-11-02 decision rejects a structural `ProjectSubgraphSelector` in the semantic
path. It requires `ProjectVirtualNodeSelector` to search only the direct virtual nodes
in the current subject scope. A selected virtual node supplies the semantic route into
its backing subgraph. The decision also replaces numeric tiled-child selection with a
named `ProjectTiledChildSelector` in the intended model.
