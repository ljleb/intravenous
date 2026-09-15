# Instance-Aware Span Query Direction

## Goal

Keep source spans definition-owned, but make span queries return instance-backed runtime nodes so the sidepanel can inspect and control a specific iv-module instance.

## Request Surface

Extend `graph.queryBySpans` with optional `instanceId`.

- `filePath`, `ranges`, `match` stay the same.
- `instanceId` narrows results to one realized instance when present.
- Without `instanceId`, the backend may return matching nodes for all realized instances.

## Runtime Identity

Return `LogicalNodeInfo.instance_id` and make `LogicalNodeInfo.id` instance-qualified.

- `instance_id` is the explicit UI/runtime instance selector.
- `id` becomes a stable runtime node id that uniquely identifies one logical node within one instance.
- The instance-qualified node id is used by existing control RPCs so they remain coherent without inventing a second node identity scheme.

## Resolution Flow

1. Match spans against definition-owned introspection indexes.
2. Resolve matching logical nodes to realized iv-module instances by `definition_id`.
3. Materialize one live logical node per matching instance.
4. Use instance-qualified node ids for control/live-value plumbing.

## App-Module Event Graph

The key runtime wiring stays directed and acyclic:

- `IvModuleDefinitionsChanged -> IvModuleSourceIntrospection`
  keeps definition indexes current
- `IvModuleInstancesListChanged -> IvModuleSourceIntrospection`
  keeps realized instance selection/resolution current
- `IvModuleSourceIntrospection -> live snapshot request event -> GraphInputLanes`
  resolves live values for one instance-qualified node at a time

## Minimal Implementation Strategy

1. Let `IvModuleSourceIntrospection` keep multiple definition indexes instead of one global index.
2. Track realized iv-module instances inside `IvModuleSourceIntrospection`.
3. Add optional `instanceId` to `graph.queryBySpans`.
4. Return `instanceId` and instance-qualified node ids from span queries.
5. Make graph-input-lane descriptors and live snapshot keys use the same instance-qualified logical node ids.

## Non-Goals For This Step

- No separate "selected instance" backend route is required.
- No replacement of definition-owned source spans.
- No broader UI selection model beyond the simple dropdown already added.
