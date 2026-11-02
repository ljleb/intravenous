# Configuration Expression Language Direction

_Status: normative Front 9 design. The parser, type metadata, evaluator, reference
resolver, and typed construction path are not implemented yet._

This document defines the restricted language for project-authored node configuration.
The language constructs typed values and refers to existing project nodes. It does not
execute general C++.

The earlier candidate syntax and matcher semantics are preserved in
[Front 9 Configuration Reference Syntax Before 2026-11-02](./historical/front_9_configuration_reference_syntax_before_2026-11-02.md).

## Role

A configuration source is an argument list for one registered node configuration
signature. The registered signature supplies the expected type of each argument.

Configuration expressions are not the primary user interface for most nodes. The
application can provide these interfaces:

- Generic controls generated from configuration metadata.
- A direct expression editor.
- A custom Vue presentation.
- Typed node messages from Fronts 13 and 14.

A custom presentation can update construction arguments without asking the user to
edit source text. The project still persists the declarative configuration request.

The language must support these operations:

- Construct aggregates, containers, scalar values, strings, and enum values.
- Pass existing project nodes as construction arguments.
- Refer to semantic descendants of existing project nodes.
- Preserve structured values for later argument updates.

The language does not contain statements, declarations, assignments, loops, lambdas,
templates, allocation APIs, or arbitrary function calls. It cannot access the file
system or network.

## Type-directed argument lists

The parser reads the complete argument list. Application code must not split the source
at commas.

For example, a registered signature can accept this source:

```cpp
{.cutoff = 1800, .q = 0.7}, stereo
```

The expected parameter types determine the aggregate type and the enum type. The source
does not need redundant type names or numeric suffixes when the target type gives the
required information.

The language supports these forms when the expected type permits them:

- Numeric, Boolean, and string literals.
- Arithmetic and parentheses.
- An allowlist of pure mathematical functions.
- Named and positional aggregate initialization.
- Nested aggregates and registered containers.
- Bare enum constants.
- Stable project-node references and semantic node paths.

Named aggregate initialization can omit an explicit aggregate type:

```cpp
{
    .cutoff = 1800,
    .mode = lowpass,
    .bands = {
        {.frequency = 200, .gain = -3},
        {.frequency = 2000, .gain = 2.5}
    }
}
```

The evaluator uses C++ initialization and default rules for every supported type. It
must not invent a value for an omitted required member.

The language has no local variables and no general structure-member expression. In
`{.cutoff = 1800}`, `.cutoff` is an aggregate member designator. In
`&550e8400-e29b-41d4-a716-446655440000.filter`, `.filter` is a semantic node selector.
These forms are different grammar productions.

## Enum values

An enum value is a bare identifier. The expected enum type controls lookup.

For example, if a parameter expects `Mode`, the source can contain `stereo`. If an
aggregate field expects `FilterMode`, the source can contain `.mode = lowpass`.

The language does not give namespace `iv` special behavior. Framework and package enum
types use the same mechanism. `Type::member` is not part of the initial language.

The Clang package-introspection path must emit metadata for each enum type reachable
from a registered configuration signature. Reachability includes enum types in nested
aggregate fields and registered containers. The metadata contains:

- The enum type identity and definition fingerprint.
- Its underlying integral representation.
- Each enumerator name and value.
- Provider-generated operations that construct a valid typed enum value.

The parser must not read arbitrary C++ headers. An identifier without a supported
expected type must produce a diagnostic. It must not use global C++ symbol lookup.

## Stable project-node references

A node-ID literal starts with `&` and contains one durable project node ID:

```cpp
&550e8400-e29b-41d4-a716-446655440000
```

The lexer reads the complete ID as one token, including its hyphens. The exact lexical
format must match the durable node-ID contract from Front 10.

The literal stores a stable authored project identity. It does not store a `NodeRef`, a
builder handle, an execution node index, or another generation-local identity.

A generic editor can display a friendly node name and let the user select a graph node.
The persisted source retains the stable project node ID.

## Semantic node paths

A semantic node path starts with a node-ID literal. It then applies zero or more postfix
selectors:

```text
node_path := node_id_literal node_path_suffix*

node_path_suffix :=
      "." identifier
    | "[" integer "]"
    | "#" identifier
```

Each selector maps to one existing project matcher concept:

| Syntax | Record | Operation |
| --- | --- | --- |
| `&uuid` | Stable project node ID | Establish the authored project-node subject. |
| `.name` | `ProjectVirtualNodeSelector` | Select direct virtual nodes named `name` in the subject scope. |
| `[n]` | `ProjectVirtualMemberSelector` | Select concrete member `n` of a virtual node. |
| `#name` | `ProjectTiledChildSelector` | Select the tiled child with semantic name `name`. |

For example:

```cpp
&550e8400-e29b-41d4-a716-446655440000.processor
&550e8400-e29b-41d4-a716-446655440000.processor.voices[2]
&550e8400-e29b-41d4-a716-446655440000.processor.voices[2]#left
```

The three postfix selectors are distinct. A virtual node can contain multiple concrete
members. A concrete member can also be tiled. Therefore `[2]` and `#left` cannot share
one representation.

`#name` always requires a subject. It is not a global virtual-node query. Tile names are
semantic names such as `left`, `right`, `middle`, and `side`. They are not numeric tile
positions.

Port selection is not part of this path grammar. If configuration expressions later
refer to ports, the design must keep port selection and port-channel selection distinct
from these three selectors.

## Scope-local virtual-node lookup

`ProjectVirtualNodeSelector` selects only virtual nodes that belong directly to the
current subject subgraph. It must not search descendant subgraphs.

Consider this hierarchy:

```text
Root
|-- processorA
|   `-- filter
`-- processorB
    `-- filter
```

A `.filter` selector on `Root` selects neither `filter`. These paths select them:

```cpp
&550e8400-e29b-41d4-a716-446655440000.processorA.filter
&550e8400-e29b-41d4-a716-446655440000.processorB.filter
```

This rule makes each subgraph a semantic namespace. It also prevents virtual nodes in
unrelated descendants from changing a parent lookup result.

If several direct virtual nodes match one `ProjectVirtualNodeSelector`, the result stays
set-valued. A consumer that requires one result must make sure that the cardinality is
one.

## Subgraph navigation

Each semantically addressable subgraph must have a backing virtual node. Selecting that
virtual node makes its backing subgraph available to the next selector. A semantic path
does not need a separate structural step to enter the subgraph.

`ProjectSubgraphSelector` is not part of the intended semantic node-path model. The
graph still contains subgraph bundles, scopes, and construction records. This decision
only removes structural kind-and-index navigation from user-facing semantic identity.

An unnamed internal subgraph is not addressable through its incidental structural
position. If a project must address it, its author must give it a backing virtual node.

## Shared resolution with `GraphConnections`

Configuration references and `ProjectNodePortMatcher` must use the same semantic path
resolver. The configuration reference stops at a node. A connection matcher continues
with its separate port matcher and optional port-channel selector.

The shared resolver performs these operations:

1. Establish the stable authored project-node subject.
2. Apply each `ProjectVirtualNodeSelector`, `ProjectVirtualMemberSelector`, or
   `ProjectTiledChildSelector` in order.
3. Keep virtual-node lookup local to the current subject scope.
4. Preserve set-valued results.
5. Return stable semantic matches, not persisted concrete graph positions.

The configuration evaluator then makes sure that the result is valid for the expected
construction-argument type.

The current `GraphConnections::apply_path_selector()` behavior differs from this
design. It searches descendant virtual-node records, uses a numeric
`ProjectTiledChildSelector`, and accepts `ProjectSubgraphSelector`. Front 9 must replace
or refactor that behavior instead of creating a second resolver with different rules.

## Structured values and updates

The expression compiler produces structured typed values. It retains field and index
structure that a later Front 13 operation can use to replace one subvalue while it
preserves all other configured arguments.

For example, a later operation can replace the frequency of `voices[1]` in this value:

```cpp
{
    .voices = {
        {.frequency = 440, .gain = 0.5},
        {.frequency = 660, .gain = 0.25}
    },
    .master_gain = 0.8
}
```

This capability requires metadata for aggregate fields, registered containers, and
safe typed construction. It does not add assignments or mutable variables to the
expression language. The evaluator must use generated operations. It must not infer an
erased C++ object layout.

## Ownership, dependencies, and caches

The implementation keeps these facts separate:

- The parsed source and its stable semantic references.
- Resolution against one project graph state.
- Materialization as owned typed construction arguments.
- The configured node result and its graph-resource ownership.

A stable project node ID is not a `NodeRef`. Retaining the ID cannot make a builder
handle valid across `BuilderSession` objects.

If materialization requires a live graph handle across `BuilderSession` objects, that
part depends on Front 12. Front 9 must not create an unsafe cross-session handle as a
temporary substitute. Front 9 can land the syntax, stable representation, diagnostics,
and project resolution before that live construction path.

The implementation also keeps these caches separate:

- A syntax and type-analysis cache for configuration source.
- Project-state-dependent semantic reference resolution.
- The existing `NodeInstances` cache for reusable configured node values.

An expression source and definition version do not identify the result of project-node
reference resolution. Project revision and semantic dependencies must participate in
reuse or invalidation.

## Editable project subgraphs

Front 9 does not implement the future project-owned editable-subgraph system. It must
remain compatible with these rules:

- An authored subgraph has a stable identity.
- An addressable subgraph has a backing virtual node.
- Virtual-node lookup is local to one subgraph.
- A path enters a subgraph through its selected virtual node.
- Referring to an existing subgraph differs from constructing a new reusable instance.

`ProjectGraph` and its existing child modules will own the future project-editable
subgraph representation.

## Required tests

Front 9 tests must cover these cases:

- Type-directed scalar, aggregate, nested aggregate, and container construction.
- Framework and package enum values with generated metadata.
- The same enumerator name in two unrelated enum types.
- A stable project-node ID literal with hyphens.
- `.name`, `[n]`, and `#name` in one path.
- Same-named virtual nodes in sibling subgraphs.
- A parent lookup that must not find a descendant-only virtual node.
- Set-valued direct virtual-node matches and cardinality diagnostics.
- Unknown project IDs, member indices, and tile names.
- Rejection of a standalone `#name`.
- Shared path results between configuration references and `GraphConnections`.
- Project-dependent cache invalidation for resolved references.

## Open implementation details

The following details remain open:

- The canonical lexical format of a durable project node ID.
- The ABI for configuration type metadata and typed construction.
- The metadata representation for named tiled children.
- The exact application of selectors to set-valued intermediate results.
- The safe conversion of an authored-node reference into a construction argument
  across builder scopes.
- Project-dependent dependency tracking and cache invalidation.
- The durable representation of future project-owned editable subgraphs.

These details cannot reintroduce recursive virtual-node lookup, implicit global lookup,
ambiguous indexing, or redundant type syntax.
