"""
You’re right. I meant “current pasted unit,” not filenames.

Here are the sensible groups of similar intent I’d split this into. No filenames.

| Group                                                      | Belongs here                                                                                                                                                                                                 | Why                                                                                                                                                                                                                             |
| ---------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **1. Public lightweight reference handles**                | `SamplePortRef`, `EventPortRef`, `NodeRef`, `TypedNodeRef`, `StructuredNodeRef`, `NodeRefBase`                                                                                                               | These are user-facing graph handles. Their job is to point at graph/node/port state and expose ergonomic operations like indexing outputs, connecting inputs, cloning handles, detaching, and stringifying.                     |
| **2. Named argument / syntax sugar layer**                 | `fixed_string`, `NamedArg`, `PortName`, `NamedRef`, `OutputRefConfig`, `EventOutputRefConfig`, named output/input helpers                                                                                    | This is purely call-site ergonomics: named ports, named outputs, compile-time names, initializer-list naming, and named argument validation. It should not live near graph lowering or metadata.                                |
| **3. Compile-time node-shape traits**                      | `fixed_input_count`, `fixed_output_count`, event input/output count traits, `should_preserve_node_type_v`, `node_ref_for_t`                                                                                  | This is template introspection for deciding whether a node has fixed arity and what kind of reference type should be returned. It is type-system machinery, not graph-building logic.                                           |
| **4. Node call argument validation**                       | `is_named_arg`, `unique_named_args`, `named_args_follow_positionals_only`, `arg_targets_sample_input_v`, `arg_targets_event_input_v`, `node_call_enabled`                                                    | This is a separate compile-time validation layer for `node(...)` / `operator()(...)` calls. It enforces call syntax rules and input count constraints.                                                                          |
| **5. Builder node storage model**                          | `BuilderNode` and its accumulated fields: configs, materialization, TTL, subgraph bookkeeping, source info, logical IDs, type identity                                                                       | This should become a set of encapsulated subobjects. Right now it mixes port config, materialization, lifetime, lowered-subgraph data, source metadata, logical grouping metadata, and vacant-input ownership.                  |
| **6. GraphBuilder core state and identity**                | Builder identity, `_nodes`, `_edges`, `_event_edges`, public inputs/outputs, placed ports, detach IDs, root/nested builder identity                                                                          | This is the core mutable graph assembly state. It should be kept distinct from syntax sugar, metadata, and lowering.                                                                                                            |
| **7. Public graph input/output declaration API**           | `input(...)`, `event_input(...)`, `outputs(...)`, `event_outputs(...)`, output validation, scope-aware input/output declaration                                                                              | This is the user-facing boundary declaration layer: declaring public graph ports and exported outputs. It has enough logic to justify being separated from node insertion.                                                      |
| **8. Node insertion and materialization API**              | `node<Node>(...)`, `validate_output_port_configs`, node value construction, `materialize` lambda, return-type selection                                                                                      | This is the “add concrete node to builder” path. It deals with node construction, input/output config extraction, type erasure, detach-node special casing, and returned ref type selection.                                    |
| **9. Connection operations**                               | `connect_sample_input`, `connect_event_input`, `NodeRefBase::connect_input`, `connect_event_input`, `operator()`, `input_is_connected`, placement tracking                                                   | This is graph wiring logic. It should own the rules for sample/event edge creation, validating source/target compatibility, and detecting duplicate/filled inputs.                                                              |
| **10. Detach / feedback-loop support**                     | `detach_sample_port`, `SamplePortRef::detach`, detached writer/reader info, `_detached_info_by_source`, `_detached_reader_outputs`, detach ID offset handling                                                | Detach is a specialized feature with its own lifecycle and remapping rules. It should not be mixed into generic node insertion or subgraph embedding except through a narrow interface.                                         |
| **11. Scoped subgraph construction**                       | `ScopedSubgraph`, `subgraph(...)`, `define_scope_outputs`, `define_scope_event_outputs`, placeholder input nodes, lowered-subgraph placeholder creation                                                      | This is one of the biggest independent responsibilities. It handles temporary scope state, placeholder nodes, translation of internal edges, and exposure of subgraph boundaries.                                               |
| **12. Embedded child graph lowering/remapping**            | `derive_nested_builder`, `embed_subgraph`, child node copying, port remapping, detach offset remapping, edge remapping                                                                                       | This is related to subgraphs but distinct: it imports an already-built child graph into a parent. It is mostly graph remapping/lowering logic.                                                                                  |
| **13. Vacant/runtime-filled input handling**               | `VacantSampleInput`, `VacantEventInput`, `VacantInputs`, `vacant_inputs`, `mark_runtime_filled_sample_input`, `mark_runtime_filled_event_input`, vacant input owner fields                                   | This is runtime patch-point discovery. It is not normal graph building and should be isolated as an inspection/late-binding concern.                                                                                            |
| **14. Source annotation and logical declaration tracking** | `_annotate_source_info`, `_annotate_node_source_info`, `_add_node_logical_id`, `_initialize_node_vacant_input_owner`, `_transfer_node_vacant_input_owner`                                                    | This is source-mapping / logical-node tracking. It exists for introspection and tooling, not audio/event graph construction.                                                                                                    |
| **15. Introspection metadata construction**                | `LogicalConcretePortInfo`, `LogicalConcreteNode`, `MetadataGraphNode`, `build_logical_metadata`, `aggregate_ports`, `aggregate_connectivity`, `source_spans_for`, `stable_identity_suffix`, `build_metadata` | This is a full metadata pipeline. It builds logical nodes from concrete/lowered nodes, aggregates ports, computes connectivity, groups by source identity/type identity, and sorts results. It clearly deserves its own module. |
| **16. Build/finalization pipeline**                        | `build_root_node`, `GraphIntrospectionMetadata build_metadata`, prepared graph creation, public output checks, final graph object construction                                                               | This is the final compilation/export step from builder state into executable graph/introspection structures. It should consume builder state but not own the public builder API.                                                |
| **17. Small utility/error helpers**                        | `demangle_type_name`, `event_type_name`, `sort_and_deduplicate_spans`, `allocate_root_builder_id`, string formatting helpers                                                                                 | These are support functions. Some belong near metadata, some near diagnostics, but they should not be embedded inside the main builder type.                                                                                    |

The highest-value split is:

1. **Reference handles**
2. **Named-argument syntax**
3. **Template traits and validation**
4. **Builder core state**
5. **Node insertion**
6. **Connection/wiring**
7. **Public I/O declaration**
8. **Subgraph scope handling**
9. **Child graph embedding/remapping**
10. **Detach support**
11. **Vacant/runtime-filled input support**
12. **Source/logical metadata annotation**
13. **Introspection metadata generation**
14. **Final graph build/export**

The most urgent extraction is `BuilderNode`: it currently combines ordinary node config, materialization, TTL, lowered-subgraph state, source info, logical IDs, vacant-input ownership, type identity, and subgraph kind in one flat struct. That should become composed state with separate semantic owners. The pasted unit itself flags the same issue around `BuilderNode`, and the visible declarations show those mixed responsibilities directly.
"""

"""
Yes. I’d split it into **three levels**:

1. **Pure headers / modules**: template traits, named-argument helpers, refs.
2. **Stateful helper classes composed into `GraphBuilder`**: subgraph scopes, detach bookkeeping, public I/O, metadata annotation.
3. **Implementation-only services**: metadata builder, graph finalizer, child-graph embedder.

A good target shape:

```cpp
class GraphBuilder {
public:
    // keep existing public API
    input(...);
    event_input(...);
    node<T>(...);
    outputs(...);
    event_outputs(...);
    subgraph(...);
    embed_subgraph(...);
    vacant_inputs() const;
    build_metadata(...) const;
    build_root_node(...) const;

private:
    BuilderIdentity _identity;
    BuilderTopology _topology;

    PublicPortApi _public_ports;
    NodeInsertionApi _node_insertion;
    ConnectionApi _connections;
    SubgraphScopeManager _subgraphs;
    DetachManager _detach;
    VacantInputTracker _vacant_inputs;
    SourceAnnotationStore _source_annotations;
};
```

But not everything needs to be a class. Some things are just type utilities.

## 1. Make these separate modules, not classes

| Group                       |                              Form | Reason                                                                                                                                                                                 |
| --------------------------- | --------------------------------: | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Reference handles           |                        Own module | `SamplePortRef`, `EventPortRef`, `NodeRefBase`, `NodeRef`, `TypedNodeRef`, `StructuredNodeRef` are public user-facing types. They should be readable without seeing the whole builder. |
| Named argument syntax       |                        Own module | `fixed_string`, `NamedArg`, `PortName`, `NamedRef`, `OutputRefConfig`, `EventOutputRefConfig` are syntax-layer helpers. No state needed.                                               |
| Node shape traits           |                        Own module | `fixed_input_count`, `fixed_output_count`, event count traits, `node_ref_for_t` are pure type machinery.                                                                               |
| Node call validation        |                        Own module | `is_named_arg`, `unique_named_args`, `sample_input_arg_count_v`, `event_input_arg_count_v`, `node_call_enabled` are compile-time validation only.                                      |
| Small diagnostics/utilities | Own implementation utility module | `demangle_type_name`, `event_type_name`, span sorting, stable hash suffix. Some may be private to metadata.                                                                            |

These should mostly be header-only because they are templates or tiny inline helpers.

## 2. Turn `BuilderNode` into composed state

`BuilderNode` is the most obvious class extraction target. Right now it is a flat bag of unrelated state: port configs, materialization, TTL, lowered-subgraph data, source info, logical IDs, vacant-input owner, type identity, and subgraph kind. The pasted file itself calls out that this has accumulated orthogonal state and should be split into encapsulated subobjects. 

A better shape:

```cpp
struct BuilderNode {
    NodePorts ports;
    NodeMaterialization materialization;
    NodeLifetime lifetime;
    LoweredSubgraphBinding lowered_subgraph;
    NodeSourceAnnotations source;
    LogicalNodeBinding logical;
    VacantInputOwnership vacant_inputs;
    NodeTypeIdentity type;
};
```

Where:

```cpp
struct NodePorts {
    std::vector<InputConfig> sample_inputs;
    std::vector<OutputConfig> sample_outputs;
    std::vector<EventInputConfig> event_inputs;
    std::vector<EventOutputConfig> event_outputs;

    std::span<InputConfig const> inputs() const;
    std::span<OutputConfig const> outputs() const;
    std::span<EventInputConfig const> event_inputs_view() const;
    std::span<EventOutputConfig const> event_outputs_view() const;
};
```

```cpp
struct NodeMaterialization {
    std::function<TypeErasedNode(size_t)> materialize;

    bool is_placeholder() const;
    TypeErasedNode make(size_t detach_id_offset) const;
};
```

```cpp
struct LoweredSubgraphBinding {
    std::string kind;
    size_t begin = 0;
    size_t count = 0;

    std::vector<std::vector<PortId>> sample_input_targets;
    std::vector<PortId> sample_output_sources;
    std::vector<std::vector<PortId>> event_input_targets;
    std::vector<PortId> event_output_sources;

    bool active() const;
};
```

```cpp
struct NodeSourceAnnotations {
    std::vector<SourceInfo> source_infos;
};

struct LogicalNodeBinding {
    std::vector<std::string> logical_node_ids;
};

struct VacantInputOwnership {
    std::string owner_logical_node_id;
};

struct NodeTypeIdentity {
    std::string type_identity;
};
```

That lets the final builder code say things like:

```cpp
_nodes[index].ports.outputs()
_nodes[index].lowered_subgraph.active()
_nodes[index].source.add(...)
_nodes[index].logical.add(...)
```

instead of reaching into every field directly.

## 3. Make these stateful utility classes inside `GraphBuilder`

These should likely be real classes, because they own state and enforce invariants.

### `BuilderTopology`

Owns the canonical graph topology under construction: builder nodes plus sample/event edges.

```cpp
class BuilderTopology {
public:
    size_t append_node(BuilderNode node);
    BuilderNode& node(size_t index);
    BuilderNode const& node(size_t index) const;

    void add_sample_edge(GraphEdge edge);
    void add_event_edge(GraphEventEdge edge);

    std::span<BuilderNode const> nodes() const;

private:
    std::vector<BuilderNode> _nodes;
    std::unordered_set<GraphEdge> _edges;
    std::unordered_set<GraphEventEdge> _event_edges;
};
```

This becomes the shared substrate used by the other helpers.

### `PublicPortRegistry`

Owns public graph inputs/outputs.

```cpp
class PublicPortRegistry {
public:
    SamplePortRef add_sample_input(GraphBuilder&, std::string_view name, Sample default_value);
    EventPortRef add_event_input(GraphBuilder&, std::string_view name, EventTypeId type);

    void define_sample_outputs(...);
    void define_event_outputs(...);

    bool outputs_defined() const;

private:
    std::vector<InputConfig> _sample_inputs;
    std::vector<EventInputConfig> _event_inputs;
    std::vector<OutputConfig> _sample_outputs;
    std::vector<EventOutputConfig> _event_outputs;
    bool _outputs_defined = false;
};
```

This isolates public graph boundary logic from node insertion.

### `NodeFactory` / `NodeInsertion`

This can be either a stateless service or a small class with references to `BuilderTopology`.

```cpp
class NodeInsertion {
public:
    template<class Node, class... Args>
    details::node_ref_for_t<Node> add(GraphBuilder&, Args&&... args);

private:
    template<class Config>
    static void validate_output_port_configs(...);

    template<class StoredNode>
    static NodeMaterialization make_materializer(StoredNode node);
};
```

This owns:

* constructing the node value
* reading `get_inputs`, `get_outputs`, `get_event_inputs`, `get_event_outputs`
* validating multi-output names
* building the `TypeErasedNode` materializer
* choosing `NodeRef`, `TypedNodeRef`, or `StructuredNodeRef`

### `ConnectionManager`

Owns wiring rules.

```cpp
class ConnectionManager {
public:
    void connect_sample_input(PortId target, SamplePortRef source);
    void connect_event_input(PortId target, EventPortRef source);

    bool sample_input_is_connected(PortId target) const;
    bool event_input_is_connected(PortId target) const;

    void mark_sample_input_placed(PortId target);
    void mark_event_input_placed(PortId target);

private:
    std::unordered_set<PortId> _placed_sample_inputs;
    std::unordered_set<PortId> _placed_event_inputs;
};
```

This is where duplicate connection checks and source/target validation belong.

### `DetachManager`

This should definitely be its own class.

```cpp
class DetachManager {
public:
    SamplePortRef detach(GraphBuilder&, SamplePortRef source, size_t loop_extra_latency);

    size_t reserve_child_detach_ids(size_t child_count);
    void import_child_detaches(...);

    DetachedSamplePortInfo const* find(PortId source) const;

private:
    size_t _next_detach_id = 0;
    std::unordered_map<PortId, DetachedSamplePortInfo> _detached_info_by_source;
    std::unordered_set<PortId> _detached_reader_outputs;
};
```

Detach has specialized ID offset logic, reader/writer bookkeeping, and child graph remapping. Keeping that inside `GraphBuilder` makes unrelated code harder to reason about.

### `SubgraphScopeManager`

This should be a stateful class.

```cpp
class SubgraphScopeManager {
public:
    bool active() const;
    ScopedSubgraph& current();

    SamplePortRef add_scoped_sample_input(...);
    EventPortRef add_scoped_event_input(...);

    void define_sample_outputs(...);
    void define_event_outputs(...);

    NodeRef run_scope(GraphBuilder&, Fn&& fn, std::string_view kind);

private:
    std::vector<ScopedSubgraph> _scope_stack;
};
```

It owns:

* `_scope_stack`
* `ScopedSubgraph`
* scoped input placeholder nodes
* scoped output definitions
* placeholder lowering
* edge translation when scope exits

This is one of the cleanest extractions because the behavior is already conceptually isolated.

### `ChildGraphEmbedder`

This can be a stateless implementation service taking references to parent/child builders.

```cpp
class ChildGraphEmbedder {
public:
    static NodeRef embed(GraphBuilder& parent, GraphBuilder const& child);
};
```

It owns:

* child output validation
* child detach offset reservation
* child node copying
* sample/event edge remapping
* lowered-subgraph placeholder remapping
* placed/runtime-filled input remapping

This does not need to be part of the public builder class.

### `VacantInputTracker`

This should be separate because it is not ordinary graph construction.

```cpp
class VacantInputTracker {
public:
    struct VacantSampleInput { ... };
    struct VacantEventInput { ... };
    struct VacantInputs { ... };

    VacantInputs collect(...) const;

    void mark_runtime_filled_sample_input(PortId target);
    void mark_runtime_filled_event_input(PortId target);

private:
    std::unordered_set<PortId> _timeline_filled_sample_inputs;
    std::unordered_set<PortId> _timeline_filled_event_inputs;
};
```

It needs access to node ports, edges, and logical ownership, but it does not need to live directly in `GraphBuilder`.

### `SourceAnnotationManager`

Owns source spans and logical declaration binding.

```cpp
class SourceAnnotationManager {
public:
    void annotate(
        GraphBuilder&,
        NodeRef const&,
        std::string_view declaration_identity,
        std::string_view file_path,
        uint32_t begin,
        uint32_t end
    );

    void add_logical_id(BuilderNode&, std::string_view logical_node_id);
    void initialize_vacant_input_owner(BuilderNode&, std::string_view logical_node_id);
    void transfer_vacant_input_owner(BuilderNode&, std::string_view logical_node_id);
};
```

This separates source mapping and logical-node bookkeeping from graph construction.

### `IntrospectionMetadataBuilder`

This should be entirely separate from `GraphBuilder`.

```cpp
class IntrospectionMetadataBuilder {
public:
    static GraphIntrospectionMetadata build(
        PreparedGraph const& graph,
        std::span<LoweredSubgraphSpec const> lowered_scopes
    );
};
```

It owns:

* `LogicalConcretePortInfo`
* `LogicalConcreteNode`
* `MetadataGraphNode`
* `aggregate_connectivity`
* `aggregate_ports`
* `source_spans_for`
* logical grouping
* sorting
* backing-node-to-logical-node mapping

This is currently a major independent pipeline embedded under `details`. It should be extracted.

### `GraphFinalizer`

Owns final export.

```cpp
class GraphFinalizer {
public:
    static Graph build_root_node(GraphBuilder const&, size_t detach_id_offset);
    static PreparedGraph prepare(GraphBuilder const&, size_t detach_id_offset);
};
```

This consumes builder state and produces the executable graph / prepared graph. It should not be mixed with public graph-construction API.

## 4. What stays directly on `GraphBuilder`

`GraphBuilder` should keep the **facade** methods only:

```cpp
class GraphBuilder {
public:
    GraphBuilder();

    GraphBuilder derive_nested_builder();

    SamplePortRef input();
    SamplePortRef input(std::string_view name, Sample default_value = 0.0);

    EventPortRef event_input(std::string_view name, EventTypeId type);
    EventPortRef event_input(EventTypeId type);

    template<class Node, class... Args>
    auto node(Args&&... args);

    template<class... Refs>
    void outputs(Refs&&... refs);

    template<class... Refs>
    void event_outputs(Refs&&... refs);

    template<class Fn>
    NodeRef subgraph(Fn&& fn, std::string_view kind = "Subgraph");

    NodeRef embed_subgraph(GraphBuilder const& child);

    VacantInputs vacant_inputs() const;

    void connect_sample_input(PortId target, SamplePortRef source);
    void connect_event_input(PortId target, EventPortRef source);

    GraphIntrospectionMetadata build_metadata(size_t detach_id_offset = 0) const;
    Graph build_root_node(size_t detach_id_offset = 0) const;
};
```

Internally each method delegates:

```cpp
SamplePortRef GraphBuilder::input(std::string_view name, Sample default_value)
{
    if (_subgraphs.active()) {
        return _subgraphs.add_sample_input(*this, name, default_value);
    }
    return _public_ports.add_sample_input(*this, name, default_value);
}
```

```cpp
NodeRef GraphBuilder::embed_subgraph(GraphBuilder const& child)
{
    return ChildGraphEmbedder::embed(*this, child);
}
```

```cpp
GraphIntrospectionMetadata GraphBuilder::build_metadata(size_t detach_id_offset) const
{
    return IntrospectionMetadataBuilder::build(
        GraphFinalizer::prepare(*this, detach_id_offset),
        lowered_subgraph_specs()
    );
}
```

## 5. Suggested extraction order

Do this in dependency-safe order:

1. Extract **named argument syntax** and **template traits** first. Lowest risk.
2. Extract **reference handles** while keeping friend access to `GraphBuilder`.
3. Replace flat `BuilderNode` fields with composed subobjects.
4. Extract **DetachManager**. It has clear state and narrow behavior.
5. Extract **SubgraphScopeManager**. Big payoff, but more edge-remapping risk.
6. Extract **PublicPortRegistry** and **ConnectionManager**.
7. Extract **ChildGraphEmbedder**.
8. Extract **SourceAnnotationManager** and **VacantInputTracker**.
9. Extract **IntrospectionMetadataBuilder**.
10. Extract **GraphFinalizer**.

The key rule: keep `GraphBuilder` as the compatibility facade, but stop letting it be the owner of every concept. Its public interface can remain stable while the internals become a set of named collaborators with explicit responsibilities.

Implementation-wise, the public entry point should remain `graph/builder.h`, while the internal collaborators should live in a `graph/builder/` subdirectory with short roleful filenames instead of a flat `builder_*` prefix namespace.
"""

The above two groups of text are the result of a refactoring conversation I had. It's most of the info you need to understand how the refactor should go.
