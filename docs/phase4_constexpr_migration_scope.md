# Phase 4 constexpr migration scope

Read this file completely after every context compaction before changing Phase
4 code. It is the implementation contract. If a required change is not implied
by this document or by a compiler diagnostic on the existing call chain, stop
and explain it before editing.

## Destination

Saving a module compiles its addressable
`consteval void(GraphBuilder&)` entry with GCC C++26 reflection. The existing
GraphBuilder, lowering, and whole-graph compiler execute during constant
evaluation. Their final `CompiledGraph` is frozen into fixed static storage and
its internal execution is expanded at compile-time-known indices so the C++
compiler can optimize the graph as one function.

`NodeLayout`, `NodeStorage`, `Node::declare()`, `Node::initialize()`,
`Node::move()`, and `Node::release()` remain runtime mechanisms. The generated
graph root is the only runtime `TypeErasedNode` boundary.

## Proven immediate-reflection mechanism

`GraphBuilder::node<T>(args...)` constructs the structural `T` value during
constant evaluation and reflects it immediately. It does not store, serialize,
reconstruct, register, or runtime-materialize the authored node.

The focused GCC 16 prototype established this shape:

```cpp
template <class Node, class... Args>
constexpr auto GraphBuilder::node(Args&&... args)
{
    if consteval {
        Node node(std::forward<Args>(args)...);
        auto reflected = std::meta::reflect_constant(node);
        // Substitute reflected into wrappers such as tick_node<node> and
        // retain their ordinary runtime function pointers.
        // Append the existing semantic node record and return its NodeRef.
    } else {
        // GraphBuilder has no runtime authoring path after Phase 4.
    }
}
```

The `if consteval` supplies the immediate context required by
`reflect_constant` throughout graph authoring. Module entries are `consteval`,
so authored graph construction is never runtime-callable. Do not replace this
with a byte representation, erased object pointer, node-type registry, or
deferred reflection pass.

The exact node value is the NTTP of ordinary wrapper functions for all required
node behavior and immutable queries, including declaration, initialization,
move, release, tick, block tick, skip, identity, port descriptions, latency,
block size, TTL, and skip capability. Add only the wrappers actually consumed
by the existing compiler/runtime interfaces.

## Node model

- Authored node types must satisfy the actual `reflect_constant` requirements:
  structural and copy constructible. Do not impose unrelated POD or trivial
  default-construction constraints.
- Immutable node configuration is constructor state stored in the reflected
  NTTP value.
- `inputs()`, `outputs()`, `event_inputs()`, `event_outputs()`, and other
  configuration-dependent queries are `constexpr` member functions. They are
  not required to be static.
- Move template configuration such as `Sum` arity/layout into structural
  constructor state where this removes fixed-maximum template dispatch.
- `Node::State` remains a runtime type. It must be default constructible and
  must not derive from another class. `GraphBuilder` reflects its members and
  registers their names, types, offsets, sizes, and alignment directly so hot
  reload can structurally compare state types before applying `Node::move`.

## Required migration strategy

There is one implementation of each graph responsibility:

1. Add explicit `constexpr` qualifiers along the live call chain of the
   existing GraphBuilder, lowering, and compiler.
2. Repair only the implementation rejected by the next GCC diagnostic while
   preserving its algorithm, ordering, and data flow.
3. When constant evaluation needs a definition visible, merge that existing
   `.h`/`.cpp` pair into one `.hpp`, update its includes, and delete the pair.
4. Replace `NodeMaterialization` in place with the immediate reflected
   exact-value operations. Do not create a parallel node representation.
5. Keep transient `std::vector` and other standard containers throughout
   authoring, lowering, compilation, and metadata construction.
6. Freeze only the final compiled graph and metadata that leave constant
   evaluation. Use the C++26 `std::define_static_array`,
   `std::define_static_string`, and `std::define_static_object` facilities to
   promote variable-length collections and objects, then return views into
   that static storage. Generated nodes such as `ConnectionNode` receive their
   structural static views at this final boundary and use the same exact-value
   reflection path as authored nodes. No `AuthoredSizes` pass is required
   merely because authored storage is transient.
7. Expand execution using compile-time-known indices with the smallest changes
   to the existing `Graph`, `GraphSccWrapper`, and `GraphNodeWrapper`
   responsibilities. Preserve the existing graph compiler.
8. Adapt the final compiled result at the narrow module boundary. Do not rename
   runtime events or redesign definitions, instances, reload, task-runner, or
   GraphInputLanes flows merely to accommodate the compiler.

No compatibility path or partial alternate implementation may be introduced.
Intermediate revisions may fail to compile; fix the direct destination path.

## Runtime and ABI boundary

The eventual compiled module ABI is the generated translation unit's immutable
metadata plus `create()`. Module-authored code sees only GraphBuilder and does
not implement ABI details. Change the loader/runtime only when the completed
compiled result reaches that boundary, and keep any adapter as narrow as the
existing consumers permit.

All runtime graph-input dynamism remains in the existing generated
`ConnectionNode` responsibility. `GraphInputLanes` changes fixed contribution
bindings only between task-runner passes. Do not add runtime-controlled
generated node kinds and do not use atomics where graph execution exclusion
already supplies synchronization.

No vector construction, allocation, locking, or other potentially blocking
operation may occur in `tick_block()` or another audio-thread execution path.

## Compiler and verification rules

- Project and module compilation use GCC 16 or newer with C++26 reflection.
- A GCC plugin annotates source identities and token spans in the same parse
  that compiles the module. It inserts void annotation calls after authored
  statements in both the saved function body and GCC's constexpr body.
- The plugin owns only information unavailable through C++ reflection. It must
  not register node types or `Node::State` structure.
- Module imports are generated forwarding headers to the original authored
  files. There is no rewritten source tree and no separate Clang parse.
- Do not use `-fimplicit-constexpr`; annotate every required function.
- Do not add compiler-selection macros or transitional conditional paths.
- Validate uncertain C++ or reflection behavior with one focused prototype
  before production edits.
- Run only one build at a time. Builds and tests use 16 jobs.
- Preserve hand-written formatting, multiline calls, declaration followed by
  branched assignment, and project-scope include ordering.

## Mandatory stop condition

Stop and report before editing when a new diagnostic or architectural issue has
more than one reasonable solution, has no demonstrated solution, or would
require a new representation/responsibility not listed here. Do not rationalize
such a branch by extending this document after the fact.

## Completion

Phase 4 is complete only when:

- the existing module authoring, lowering, compiler, and metadata construction
  execute at compile time;
- authored nodes are reflected immediately into exact-value NTTP wrappers;
- only one builder/compiler implementation remains;
- only the final compiled result uses fixed persistent storage;
- internal node execution is statically expanded without per-node type erasure;
- the generated root is the only `TypeErasedNode` boundary;
- GraphInputLanes still controls the existing fixed ConnectionNode contribution
  points;
- runtime NodeLayout, NodeStorage, and hot-reload move behavior remain intact;
- GCC builds the project, plugin, and modules in one module parse, and relevant
  tests pass.
