# Batched Node Callback Direction

_Status: planned node API, trait-normalization, and GraphJit scheduling direction. The checked-in source does not yet provide the batch context types or compiler callback anchors described here._

[DSP Execution And Storage Glossary](./dsp_execution_storage_glossary.md) defines the execution terminology used here. This document complements [Graph JIT Direction](./graph_jit_direction.md), [Sequential Port Storage And Connection Planning](./sequential_port_storage_planning.md), and [Coverage, Random Access, And Background Evaluation](./coverage_and_background_evaluation.md).

## 1. Goal

Many useful DSP nodes are too small, too branchy, or otherwise poorly shaped to
benefit from useful SIMD within one instance. Several independent instances of the
same concrete node implementation often expose the better vectorization dimension:
process the same operation for several nodes at the same sample or logical work
position.

The batching contract therefore makes **multiple node instances of one compatible
concrete realization** a first-class callback unit. It serves two independent
optimization goals:

- cross-instance SIMD when the callback can operate lane-wise over several nodes;
- instruction-cache, instruction-prefetch, branch-target, and code-locality benefits
  even when the default batch adapter simply executes several ordinary block callbacks
  consecutively.

Batching is an optimization, never a semantic requirement. GraphJit must retain a
legal one-node path for every normalized operation and may choose scalar or batched
execution independently for different occurrences of the same node type.

A batch of size one is always legal.

## 2. Authored callback vocabulary

For ordinary Tick processing, a concrete node type authors **exactly one** of:

```cpp
void tick(TickSampleContext<MyNode> const&) const;
void tick_block(TickBlockContext<MyNode> const&) const;
static void tick_block_batch(TickBlockBatchContext<MyNode> const&);
```

The authored shape states the implementation form the node developer needs. It does
not determine the compiler-visible callback set: the node traits layer always
normalizes it to both block and batch-block operations.

`tick()` remains a first-class authoring form. It is not obsolete merely because
GraphJit schedules blocks. For a pointwise node, the traits-generated batch adapter
can place lane iteration inside the sample loop, giving LLVM an explicit
cross-instance vectorization dimension. Depending on the graph and schedule, that may
be as efficient as or more efficient than an authored `tick_block()`.

A node may additionally customize skip behavior by authoring **at most one** of:

```cpp
void skip_block(SkipBlockContext<MyNode> const&) const;
static void skip_block_batch(SkipBlockBatchContext<MyNode> const&);
```

If neither is authored, the existing generated neutral/silence skip behavior remains
the semantic default. The traits layer still exposes both normalized skip operations,
so GraphJit may batch skipped nodes as readily as active nodes.

A node with authored Tock production authors **exactly one** of:

```cpp
void tock_coverage(TockCoverageContext<MyNode>&) const;
static void tock_coverage_batch(TockCoverageBatchContext<MyNode>&);
```

Each authored coverage-propagation direction independently chooses its scalar or
batched form:

```cpp
void propagate_forward_coverage(
    PropagateForwardCoverageContext<MyNode>&) const;
static void propagate_forward_coverage_batch(
    PropagateForwardCoverageBatchContext<MyNode>&);

void propagate_reverse_coverage(
    PropagateReverseCoverageContext<MyNode>&) const;
static void propagate_reverse_coverage_batch(
    PropagateReverseCoverageBatchContext<MyNode>&);
```

Where a propagation operation is compiler-generated rather than authored, that
generated operation participates in the same normalization rules.

Batch callbacks are `static`: no arbitrary lane is the distinguished `this` object.
Every instance in the batch is a peer and is available through its lane view.

## 3. Batch context shape

Every public batch context is a small range of lane proxies. It provides:

```cpp
std::size_t size() const noexcept;
bool empty() const noexcept;
Lane operator[](std::size_t lane) const noexcept;
iterator begin() const noexcept;
iterator end() const noexcept;
```

`begin()`/`end()` are part of the public contract, not just conveniences. Ordinary
lane-wise adapters and authored callbacks should be able to use normal range
iteration:

```cpp
for (auto lane : ctx) {
    auto input = lane.input<"in">();
    auto output = lane.output<"out">();
    // ...
}
```

The iterator may return a lightweight lane proxy by value; the API does not promise a
stable `Lane&` object or contiguous lane objects.

`operator[]` remains available for sample-major/lane-minor code and explicit SIMD
implementations:

```cpp
for (std::size_t i = 0; i < block_size; ++i) {
    for (std::size_t lane_index = 0; lane_index < ctx.size(); ++lane_index) {
        auto lane = ctx[lane_index];
        // process lane at logical position i
    }
}
```

### Tick batch context

Conceptually:

```cpp
template<class Node>
class TickBlockBatchContext {
public:
    class Lane {
    public:
        [[nodiscard]] Node const& node() const noexcept;
        [[nodiscard]] TickBlockContext<Node> context() const noexcept;

        template<fixed_string Name>
        [[nodiscard]] auto input() const;

        template<fixed_string Name>
        [[nodiscard]] auto output() const;

        template<std::size_t Index>
        [[nodiscard]] auto input() const;

        template<std::size_t Index>
        [[nodiscard]] auto output() const;

        [[nodiscard]] decltype(auto) state() const
        requires (!std::is_void_v<typename NodeState<Node>::Type>);
    };

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] Lane operator[](std::size_t) const noexcept;
    [[nodiscard]] iterator begin() const noexcept;
    [[nodiscard]] iterator end() const noexcept;
};
```

The forwarding accessors are intentionally shaped like `TickBlockContext<Node>`, so
an authored batch implementation does not need a second port vocabulary. `node()`
provides the configured node instance that would otherwise have been `this` in a
scalar member callback. `state()` is lane-specific.

`Index` is declaration order across the node's single input or output schema, including
both sample and event ports. Neither scalar nor batch Tick contexts expose raw public
sample/event port ranges. Fixed-arity generic algorithms expand the constexpr schema
indexes at compile time, while reflected port spans remain private implementation
bindings.

The batch API makes no public promise that node objects, states, port buffers, or lane
records are contiguous. GraphJit remains free to change their physical layout. An
implementation may internally use structure-of-arrays records, arrays of pointers, or
another representation as long as the public lane semantics are preserved.

### Skip batch context

`SkipBlockBatchContext<Node>` follows the same range/lane structure as the Tick batch
context, with each lane exposing the corresponding `SkipBlockContext<Node>` facade and
configured node instance. It is a first-class authored API even if the first users are
only the traits-generated adapters.

This permits specialized batch skip implementations to accelerate decay, state
advancement, silence generation, or other skip semantics across several instances.

### Tock and propagation batch contexts

`TockCoverageBatchContext<Node>`, `PropagateForwardCoverageBatchContext<Node>`, and
`PropagateReverseCoverageBatchContext<Node>` use the same range shape. Each lane owns
or references the exact scalar context that would have been supplied for that node:

```cpp
for (auto lane : ctx) {
    auto& scalar = lane.context();
    auto const& node = lane.node();
    // equivalent semantics to invoking the scalar callback for this node
}
```

The lane forwards the useful scalar-context accessors directly where that improves
source ergonomics. In particular, a Tock lane exposes `input<Name>()`,
`output<Name>()`, and `tock_state()` with the same meaning as
`TockCoverageContext<Node>`.

Different lanes may carry different requested/changed coverage and different
`TockState`. Coverage equality is not a batch-compatibility requirement.

## 4. Semantic equivalence of scalar and batch forms

A native batch callback is an optimized implementation of independent scalar node
invocations. For every lane, its externally visible result must be the same as the
normalized scalar operation for that lane under the same inputs, state, and
realization.

Therefore:

- lane count must not change the semantic result of a lane;
- lane ordering must not create a graph-visible dependency between lanes;
- a node instance appears at most once in one batch invocation for a given scheduled
  operation/phase;
- GraphJit may split one candidate group into several batches or scalar invocations;
- GraphJit may invoke a batch-only authored implementation with exactly one lane.

A batch implementation may of course use SIMD registers, vector libraries, shared
lookup work, or other implementation techniques across lanes. Those optimizations must
not make the graph semantics depend on which compatible neighbours happened to be
batched with a lane.

This equivalence rule is what makes scheduling fallback legal.

## 5. Trait normalization is the only authored-shape boundary

No code outside the node trait/helper layer should branch on whether a node author
wrote `tick()`, `tick_block()`, or `tick_block_batch()`.

The canonical Tick helper surface is:

```cpp
do_tick_block(...);
do_tick_block_batch(...);
```

with these derivations:

| Authored Tick form | `do_tick_block()` | `do_tick_block_batch()` |
| --- | --- | --- |
| `tick()` | loop samples using the existing pointwise transition | loop samples, then lanes |
| `tick_block()` | direct scalar callback | ordinary `for (auto lane : batch)` loop over scalar block callbacks |
| `tick_block_batch()` | invoke the native batch callback with one lane | direct native batch callback |

The `tick()`-derived batch ordering is intentional:

```cpp
for (std::size_t sample = 0; sample < block_size; ++sample) {
    for (auto lane : batch) {
        do_tick(lane.node(), lane.sample_context(sample));
    }
}
```

The inner loop consists of independent instances of the same pointwise operation at
the same logical sample position. That gives LLVM a straightforward opportunity to
vectorize across nodes. A lane-major wrapper would preserve semantics but lose much of
the reason for retaining `tick()` as an optimized authoring form.

For an authored `tick_block()`, the default batch implementation is deliberately just
the ordinary lane loop. It does not need a special SIMD facade:

```cpp
for (auto lane : batch) {
    do_tick_block(lane.node(), lane.context());
}
```

For an authored `tick_block_batch()`, the scalar helper constructs a one-lane batch
facade and calls the native batch callback. No caller needs a separate fallback.

The equivalent normalized helper pairs are:

```cpp
do_skip_block(...);
do_skip_block_batch(...);

do_tock_coverage(...);
do_tock_coverage_batch(...);

do_propagate_forward_coverage(...);
do_propagate_forward_coverage_batch(...);

do_propagate_reverse_coverage(...);
do_propagate_reverse_coverage_batch(...);
```

Skip derivation follows the same scalar/batch rules. If the node authors neither skip
form, `do_skip_block()` implements the generic skip semantics and
`do_skip_block_batch()` loops that normalized scalar helper across lanes.

Tock and propagation derivation also follows the same rule: scalar-only authors get a
lane loop; batch-only authors get a one-lane scalar adapter; compiler-generated
propagation participates as another normalized implementation rather than creating a
special GraphJit path.

`if constexpr`/concept tests for authored callback shape belong inside this trait/helper
layer only.

## 6. Compiler-visible callback anchors

Every accepted concrete node implementation should expose both normalized scalar and
batch compiler anchors for each applicable operation. Conceptually:

```cpp
struct NodeCompilerOperations {
    // ...
    tick_block;
    tick_block_batch;
    skip_block;
    skip_block_batch;
    tock_coverage;
    tock_coverage_batch;
    propagate_forward_coverage;
    propagate_forward_coverage_batch;
    propagate_reverse_coverage;
    propagate_reverse_coverage_batch;
};
```

The exact reflected ABI remains an implementation detail, but the package compiler
must retain LLVM definitions for both choices. GraphJit then selects an operation from
scheduling/cost facts rather than from authored callback syntax.

The reflected batch ABI should carry an explicit pointer/count or equivalent stable
span record for lanes. It must not assume the object representation of
`std::span`/iterators or materialize a public array of `TickBlockContext<Node>` merely
because that is convenient for C++ source. The typed package wrapper may reconstruct
lane proxies lazily from compact reflected records.

## 7. Batch compatibility

GraphJit may place primitive invocations in one native batch only when they use the
same concrete node implementation and the same callback-facing **resolved execution
realization**.

The compatibility key includes facts that can change callback shape or indexing, for
example:

- `NodeCodeKey` / concrete implementation identity;
- resolved port value types and dynamic value sizes;
- resolved port paces;
- resolved history and latency;
- effective local sample rate;
- resolved per-port block extents / invocation quantum;
- execution/SCC phase facts that affect callback-visible semantics.

Configured values, `State`, `TockState`, port addresses, and requested Coverage need
not be equal across lanes.

Consequently, instances of the same C++ node type that resolve to different FFT sizes,
pace domains, or other callback-facing realizations normally belong to different batch
classes.

This keeps authored batch code simple: it may rely on the same structural realization
for every lane without requiring a heterogeneous per-lane metadata branch for facts
GraphJit already knows statically.

## 8. Tick scheduling and batch discovery

Batching changes the preferred topological schedule because all members of a batch
must be ready before the batch callback begins.

For compatible nodes:

```text
P1 -> A1
P2 -> A2
P3 -> A3
P4 -> A4
```

GraphJit should prefer, when legal and profitable:

```text
P1 P2 P3 P4
A1 A2 A3 A4   <- one batch invocation
```

over an alternating schedule that repeatedly leaves and re-enters the same callback
implementation.

The scheduler therefore treats a compatible batch class as a locality opportunity
inside the normal dependency constraints. Candidate members of one batch must form an
antichain with respect to dependencies that have to be satisfied within that execution
phase: no batch may hide a required ordering between two lanes.

Configured virtual-node structure may provide a cheap and high-quality pre-grouping
hint. For example, several concrete members of one virtual node often share the same
type and realization. That hint is not the semantic discovery boundary. GraphJit must
also find compatible concrete nodes elsewhere in the flattened project graph.

Batching is cost-model driven. A heterogeneous graph may make a large batch class
expensive to collect because doing so delays useful dependants or increases live
storage. GraphJit may therefore choose any mixture such as:

```text
A1 A2 A3 A4 -> batch
A5          -> scalar block
A6 A7       -> another batch
```

The scalar and batch callbacks are both retained specifically so scheduling can make
that decision after whole-graph dependencies are known.

Even the synthesized batch-over-`tick_block()` path can be profitable without data
SIMD because adjacent execution improves instruction-cache locality, instruction
prefetching, branch-target locality, and often configuration/state locality.

Skip scheduling uses the same mechanism. Once activity/TTL lowering chooses a skip
operation, compatible skipped nodes may be collected into `skip_block_batch()` rather
than forcing scalar skip callbacks.

## 9. Pace-aware Tick batching

Port pacing is resolved before batch compatibility is decided. Every lane in one Tick
batch has the same callback-facing resolved pace realization and therefore the same
per-port block extents for that invocation quantum.

For pointwise `tick()`, GraphJit may form a batch only after the existing `tick()`
legality rule proves the relevant sequential ports have compatible equal pace. The
traits-generated sample-major/lane-minor adapter then preserves the normal one-sample
transition exactly.

A native `tick_block_batch()` can support a node whose ports have heterogeneous paces
within the node, just as a native `tick_block()` can. The common-realization rule means
the corresponding input/output block sizes are nevertheless consistent across lanes.

## 10. Background Tock and propagation batching

Background evaluation should use the same batching principle rather than retaining a
permanent one-node ABI distinction.

After forward or reverse accumulation determines which nodes are ready in a phase, the
background scheduler may bucket ready nodes by compatible batch class and invoke:

```cpp
do_propagate_forward_coverage_batch(...);
do_propagate_reverse_coverage_batch(...);
do_tock_coverage_batch(...);
```

Each lane still carries its own exact changed/required/requested Coverage, bindings,
and Tock state. A batch callback is not one shared Coverage over several nodes.

The existing transaction invariant generalizes from "one callback per implicated node"
to:

> In one applicable transaction phase, each implicated node participates at most once
> in the normalized operation, whether that operation is emitted as a scalar callback
> or as one lane of a batch callback.

The scheduler should use ready-frontier bucketing rather than violating propagation
ordering merely to construct a larger batch.

Small propagation callbacks are particularly good batching candidates because the
instruction-front-end overhead of repeatedly switching among node implementations may
otherwise dominate the useful work.

## 11. Replayability

Replayability should ultimately be a semantic property of the normalized Tick block
operation, not of which authoring spelling created it.

The checked-in implementation currently restricts the intrinsic replay trait to
`tick()`-only nodes. The intended later direction is to allow normalized
`do_tick_block()` to be a replay candidate when the callback-visible semantics permit
it, for example when there is no realtime `State`, no relevant history or latency,
and the deterministic/side-effect-free replay contract is satisfied.

An authored `tick_block()` does not become unreplayable merely because it mixes values
within one block. Block callbacks are invoked at legal multiples of their configured
invocation quantum; replay can align/split requested work to those quanta rather than
pretending the callback is sample-addressable.

A node that authors only `tick_block_batch()` remains scalar-replayable through the
one-lane `do_tick_block()` adapter. If several compatible replay nodes are ready
simultaneously, a later background scheduler may use the normalized batch operation as
well.

Retaining `tick()` is useful precisely because it provides stronger pointwise structure
that the traits layer can exploit for automatic cross-node vectorization. There is no
design requirement to phase it out.

## 12. Initial implementation sequence

1. Add batch context/range types with lane proxies, `size()`, `operator[]`,
   `begin()`, and `end()`.
2. Extend node concepts/validation so Tick accepts exactly one of `tick()`,
   `tick_block()`, or `tick_block_batch()`; Tock accepts scalar or batch coverage; and
   skip/propagation accept their corresponding optional scalar-or-batch forms.
3. Add the complete normalized `do_*` scalar/batch helper pairs. Keep every authored
   callback-shape `if constexpr` inside that layer.
4. Retain both scalar and batch compiler anchors in package bitcode/reflection and
   import both into GraphJit.
5. Add compatible-batch classification and deterministic ready-node grouping without
   changing scheduling semantics. Virtual-node member groups may seed candidates but
   cannot be the only discovery path.
6. Lower scalar versus batch choices from a simple conservative cost model, including
   first-class batched skip execution.
7. Extend background propagation/evaluation roots to invoke batched forward, reverse,
   and Tock operations from their ready frontiers.
8. Add measurement-driven batch sizing, SIMD/fusion decisions, storage-layout
   co-optimization, and richer replay batching only after the semantic path is covered
   by tests.

## 13. Summary invariants

1. Node authors choose the implementation shape; GraphJit sees normalized scalar and
   batched operations.
2. `tick()`, `tick_block()`, and `tick_block_batch()` are mutually exclusive authored
   Tick forms. `tick()` remains a supported optimized form.
3. `skip_block_batch()` is a first-class authored callback. With no custom skip
   callback, both normalized skip forms are still generated.
4. Batch contexts are ordinary ranges with `size()`, `operator[]`, `begin()`, and
   `end()`; lane proxies expose the corresponding scalar context and configured node.
5. A batch of size one is always legal.
6. Native batch semantics are lane-wise equivalent to normalized scalar semantics, so
   GraphJit may freely split or scalarize a candidate batch.
7. Batch membership requires the same concrete implementation and callback-facing
   resolved realization, not identical configuration/state/coverage values.
8. Virtual-node grouping is a scheduling hint, not the discovery boundary.
9. Traits-generated batching over `tick()` is sample-major/lane-minor; generated
   batching over scalar block/coverage callbacks is a straightforward lane loop.
10. Authored callback-shape detection never escapes the node trait/helper layer.
