# Representation migration inventory

This inventory assigns every field of the current lowering/preparation path to
one of four destinations:

- **AuthoredGraph**: lossless author intent.
- **ExecutableGraphIR**: closed, self-sufficient executable semantics.
- **temporary lowering state**: implementation workspace, not retained across
  the authored-to-executable boundary.
- **remove**: diagnostic/profiling-only machinery or a round-trip that the new
  representation must eliminate.

The target has exactly two semantic representations:

```text
AuthoredGraph -> ExecutableGraphIR -> Graph
```

## `LoweringWorkspace`

| Current field | Destination | Reason |
|---|---|---|
| `topology` | temporary lowering state, then ExecutableGraphIR | Its concrete nodes and edges are the basis of executable semantics, but `TopologyPortId` and `SubgraphNode` are not a public IR boundary. |
| `bundle_projections` | temporary lowering state | Needed to map authored logical ports to projected endpoints while lowering; no later compiler phase should need it. |
| `bundle_by_lowered_node` | remove; replace with forward provenance | Later code uses it to recover authored bundles. Executable nodes instead carry authored origin, source information, and virtual IDs forward. |
| `subgraph_input_of_boundary_source` | temporary lowering state | Boundary traversal is a lowering concern. The executable IR carries completed scope interfaces directly. |
| `subgraph_event_input_of_boundary_source` | temporary lowering state | Same as sample boundary mappings. |
| `detached_info_by_source` | ExecutableGraphIR | Detach is executable semantics after its topology addresses are translated to concrete addresses. |
| `detached_reader_outputs` | ExecutableGraphIR | Same. |

## `PreparedBuilderGraph`

| Current field | Destination | Reason |
|---|---|---|
| `identity` | AuthoredGraph identity, copied as graph identity into ExecutableGraphIR | The compiler needs a stable graph ID but not a reference to the builder. |
| `lowered` | remove as a retained dependency | A closed executable IR must not retain the whole lowering workspace. |
| `topology` | temporary lowering state | Consume it while lowering; do not expose it to scheduling/layout. |
| `node_bundles` | AuthoredGraph only | Required only while deriving executable-node provenance. |
| `virtual_nodes` | AuthoredGraph only | Required only while carrying virtual provenance forward. |
| `graph` | ExecutableGraphIR | It already owns reflected nodes, concrete edges, detach data, and most node provenance. |
| `runtime_node_indices` | temporary lowering state | Translation map from topology node IDs to executable node IDs. |
| `source_of` | temporary lowering state | Topology traversal helper. Its result must be represented by completed executable edges. |
| `event_source_of` | temporary lowering state | Same. |

## `details::PreparedGraph`

| Current field | Destination | Reason |
|---|---|---|
| `nodes` | ExecutableGraphIR | Final reflected executable node definitions. |
| `explicit_ttl_samples` | ExecutableGraphIR | Per-node executable semantics. |
| `node_ids` | ExecutableGraphIR | Stable executable identity; do not stringify/reparse it for compiler lookup. |
| `node_virtual_ids` | ExecutableGraphIR provenance | Forward-carried virtual-node provenance. |
| `node_source_infos` | ExecutableGraphIR provenance | Forward-carried source provenance. |
| `node_construction_order` | ExecutableGraphIR | Stable ordering/provenance used by compilation. |
| `node_kinds` | ExecutableGraphIR provenance | Introspection/debug description. |
| `node_type_identities` | ExecutableGraphIR provenance | Introspection/debug description. |
| `edges` | ExecutableGraphIR | Final sample semantics. |
| `event_edges` | ExecutableGraphIR | Final event semantics. |
| `detached_info_by_source` | ExecutableGraphIR | Final concrete-address detach semantics. |
| `detached_reader_outputs` | ExecutableGraphIR | Final concrete-address detach semantics. |

## Compilation products

| Current type/field | Destination | Reason |
|---|---|---|
| `LoweredSubgraph` | ExecutableGraphIR scopes | Scopes are hierarchy metadata over executable nodes, not pseudo nodes or a post-lowering reconstruction. |
| `GraphExecutionPlan` | compiler analysis/product | Scheduling/layout result; it must not add graph semantics. |
| `GraphBuildArtifact` | temporary construction DTO | Valid storage/freezing input, but not a semantic IR. |
| `GraphBuildMetadata` | compilation result | Derived introspection/debug product. |
| `GraphIntrospectionMetadata` | compilation result or inspection product | Not an alternate builder output path. |

## Required semantic moves

The following current late synthesis must move into authored-to-executable
lowering before `ExecutableGraphIR` is finished:

- subgraph default `Constant` insertion;
- sample/event fan-out and event fan-in routing nodes;
- dangling sample/event sink nodes;
- scope membership and scope interface construction;
- concrete-address detach translation;
- source and virtual-node provenance attachment.

After these moves, `GraphCompiler::compile(ExecutableGraphIR const&)` may
validate, sort, construct SCCs, assign buffers, calculate latency, build
dormancy groups, create wrappers, and freeze static storage. It may not insert
semantic nodes or edges and must not receive `AuthoredGraph` components.

## Profiling cleanup

All `GraphBuilderCompileProfiler` and `GraphBuilderFinalizer::count_execution_*`
methods, plus their `ModuleCompileStage` variants, are diagnostic-only and are
removed before the representation migration. Future profiling invokes ordinary
`AuthoredGraph`/`ExecutableGraphIR` operations directly.
