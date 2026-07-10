# Iv Module Source Introspection Direction

`IvModuleSourceIntrospection` is a read model for tooling and UI queries.

Its job is to:
- map source spans to logical nodes
- expose logical nodes and active regions to the UI
- enrich logical nodes with live input snapshots from other app modules

It should not:
- own iv-module instances
- own runtime graph mutation
- require manual lifecycle orchestration from `app.cpp`

Direction:
- remove the public `initialize()` lifecycle hook
- keep the module valid immediately after construction
- update it only through bridged events from owning app modules
- treat "no graph loaded yet" as a valid empty state

Implications:
- `app.cpp` should only instantiate modules, wire bridges, and start event sources
- query methods should not fail because introspection has not been manually initialized
- `IvModuleDefinitionsChanged` remains the event that updates the introspection index
- live port values continue to come from the existing snapshot request event

Behavior:
- before any relevant definition arrives, span and region queries return empty results
- node lookup requests may still fail for unknown node ids, because that is a real lookup failure
- once definitions arrive through events, introspection becomes queryable automatically
