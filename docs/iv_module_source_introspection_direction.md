# Iv Module Source Introspection Direction

> **Naming/status:** `IvModuleSourceIntrospection` remains the appropriate name.
> Only module nodes expose source graph structure for this read model; leaf nodes
> do not. Its internal event path may change as the project-graph pipeline is
> rebuilt, but the responsibility should not be generalized merely because leaf
> and module definitions share the `NodeDefinitions` namespace. It remains a read
> model and must not participate in root graph construction. See
> [project_graph_application_architecture.md](./project_graph_application_architecture.md).

`IvModuleSourceIntrospection` is the current read model for tooling and UI queries.

Its job is to:
- map source spans to logical nodes
- expose logical nodes and active regions to the UI
- expose declared continuous value-type alternatives and type-owned authored
  presentation metadata
- expose configured connectivity and virtual-port binding

It should not:
- own iv-module instances
- own runtime graph mutation
- require manual lifecycle orchestration from `app.cpp`
- claim a selected continuous value type, resolved dynamic value size, or live runtime
  value before it joins GraphJIT realization state

Direction:
- remove the public `initialize()` lifecycle hook
- keep the module valid immediately after construction
- update it only through bridged events from owning app modules
- treat "no graph loaded yet" as a valid empty state

Implications:
- `app.cpp` should only instantiate modules, wire bridges, and start event sources
- query methods should not fail because introspection has not been manually initialized
- the current compatibility `IvPackageDefinitionsChanged` diff published by `NodeDefinitions` updates the introspection index; this direct bridge is temporary until `ProjectGraph` becomes the single execution/read-model join described in the project-graph architecture
- continuous ports report their complete declared value-type domain; each registered
  value extension owns the presentation metadata for its alternative
- event ports remain separate typed records
- JSON uses `continuousInputs` and `continuousOutputs`; Sample-only field names and
  numeric-current-value fields are not part of this boundary
- selected types, resolved FFT sizes, and live values belong to a future read model
  that joins stable project-graph identity with GraphJIT runtime state

Behavior:
- before any relevant definition arrives, span and region queries return empty results
- node lookup requests may still fail for unknown node ids, because that is a real lookup failure
- once definitions arrive through events, introspection becomes queryable automatically

**Clarification (2026-11-15):** Source-focused presentation should navigate
and interact with the same configured module instance and stable semantic
paths used by the project graph. It is not an independent set of editable
nodes. Unregistered concrete leaves can have compiler records when used in a
package; that fact alone does not provide a public `IV_NODE` definition or
replace the source annotations needed to associate virtual/logical nodes
with C++ source. Source ownership limits structural deletion, not graph
connection or observation of an addressable internal port. See
[Module Interoperability And Graph Macros](./module_interoperability_and_graph_macros_direction.md).

The superseded live-value claim is preserved in
[Source Introspection With Fake Live Port Values](./historical/source_introspection_fake_live_port_values.md).

## Public input names

The IV Clang plugin uses the identifier of a local declaration as the public
port name. A call to `input()` or `event_input()` must directly initialize
that local. This rule applies to `GraphBuilder` and `SubgraphBuilder`.
It applies when source annotations are off.

```cpp
auto const frequency = g.input<iv::Sample>();
auto const beats = g.event_input(iv::EventTypeId::beat, iv::InputAccess::coverage);
```

The template arguments still select the continuous value types or a channel
type. Later references and named connections use the declared port names.
If a declaration changes name, update its named connections and saved
references.

Native code can include `intravenous/graph/programmatic_inputs.h` and call
`iv::programmatic::declare_input` or `iv::programmatic::declare_event_input`.
These functions take a name explicitly and can read it from a runtime schema.
The plugin uses the same functions to create ports for authored declarations.
[Earlier naming forms](./historical/module_input_names_before_identifier_declarations_2026-11-21.md)
