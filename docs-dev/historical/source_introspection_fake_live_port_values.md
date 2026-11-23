# Source Introspection With Fake Live Port Values

_Status: historical. This text was removed from the current source-introspection
direction on 2026-10-16._

The earlier source-introspection direction assigned this responsibility to
`IvModuleSourceIntrospection`:

> enrich logical nodes with live input snapshots from other app modules

It also stated:

> live port values continue to come from the existing snapshot request event

The implementation did not contain such a snapshot request path. It copied each
authored default into a field named `current_value`, always reported that no concrete
override existed, and exposed read-only controls based on those values. The fields did
not describe live runtime state.

The replacement design keeps source introspection limited to facts available before
GraphJIT realization. It reports declared continuous value-type alternatives,
type-owned authored presentation metadata, connectivity, and virtual-port binding.
Selected value types, resolved dynamic sizes, and live runtime values require a future
read model that joins project-graph identity with GraphJIT state.

See [Iv Module Source Introspection Direction](../iv_module_source_introspection_direction.md)
for the current boundary.
