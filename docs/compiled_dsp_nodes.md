Below is the design we converged on for **compiled DSP ports and compiled-data execution**. This is intended to be concrete enough to implement from, while leaving storage/layout decisions to the compiler/runtime rather than baking them into the node API.

## Implementation staging

The first compiled-port implementation is deliberately limited to the
node-facing contract:

* `InputConfig::compiled` / `OutputConfig::compiled`, independent of whether
  the config contains sample or event properties;
* the sample-input `neutral_value` used by total arbitrary sample reads;
* compiled-sample callback traits, static-declaration validation, `AccessRequest`
  types, and sample access/block-access propagation contexts.

The `compiled` flag is preserved by the semantic/configured graph and its
archive. This stage does not yet add graph lowering, query planning,
materialization, or compiled-state lifecycle storage.

Compiled capability is orthogonal to port kind. The ordinary DSP-node model
therefore has all four declaration combinations:

| Port kind | Realtime access | Compiled-capable access |
| --- | --- | --- |
| sample | sequential/current-block samples | arbitrary global sample access |
| event | sequential/current-block events | arbitrary global event-range access |

The callback/context API implemented in this stage is specifically for
**compiled sample ports**. Its contexts contain compact lists of compiled sample
ports only; they never contain placeholder realtime ports. Compiled event ports
are nevertheless valid declarations and remain a distinct planned capability.
Their eventual arbitrary-access API must operate on event ranges/event sets, not
reuse the sample-grid `AccessRequest` semantics merely because both capabilities
share the `compiled` flag.

Legacy lane nodes already support both compiled sample and compiled event data.
As lane nodes are phased out, ordinary DSP nodes must preserve that capability
rather than making `compiled` sample-specific.

## 1. Meaning of a compiled port

A compiled port is not a separate kind of node and does not imply a particular buffer/cache implementation.

A **compiled output** is an output whose data can be requested independently of sequential realtime advancement. For a sample port, that means arbitrary global sample positions. For an event port, that means arbitrary global time/sample-index intervals containing timestamped events.

A **compiled input** extends the corresponding normal realtime input API with random access to its source. In `tick()` / `tick_block()`, the author should not need to remember whether an input is compiled merely to use its current-block data:

```cpp
auto in = ctx.input<"in">();
```

The statically known port declaration determines the C++ type returned by `input<>()`.

For a compiled input, that returned type supports everything the corresponding ordinary realtime input supports for the current block, plus kind-appropriate arbitrary access. A compiled sample input adds global sample-position/range access; a compiled event input adds global event-range access.

For a sample input, conceptually:

```cpp
auto x = ctx.input<"ir">();

x[i];                  // current-block access
x.at(global_index);    // arbitrary global access
x.size();               // finite logical extent
```

The precise API can differ, but the important rule is that **compiled extends realtime access rather than replacing it with a completely separate accessor vocabulary**.

A compiled output similarly behaves like an ordinary output during realtime ticking. Merely declaring an output compiled must not force it to be stored in a persistent dynamically allocated buffer.

> Compiled is a capability/demand, not a storage class.

If a compiled-capable output is only consumed sequentially, the compiler should remain free to lower it just like an ordinary realtime edge, including direct forwarding, fusion, stack/arena scratch, etc.

---

## 2. Compiled sample extent and out-of-range reads

Compiled sample values have a finite logical sample extent. Consumers need this so, for example, a convolution node can determine the length of an impulse response without guessing where it ends.

Sample reads should be total: requesting a sample outside the logical extent, in a disconnected region, or otherwise unsupported still returns a value.

The default is the input port's explicit `neutral_value`, initially `0`.
This is distinct from the ordinary disconnected sequential `default_value`, even
when both use zero by default.

The ordinary DSP implementation therefore does not need pervasive availability checks.

---

## 3. Compiled sample random-access requests are sampling requests

This section is specifically about **sample ports**. Compiled sample access is not limited to dense contiguous ranges.

The UI is an important consumer: it may display many compiled lanes over a very large time interval but need only one sample or aggregate display point per pixel. It must not have to request megabytes of dense audio merely to discard 99% of it.

A fundamental access request therefore specifies something equivalent to:

```cpp
struct AccessRequest {
    SampleIndex begin;
    SampleIndex end;
    std::size_t sample_count;
};
```

The requested sample positions are equally spaced across the interval and mapped deterministically to **exact integer global sample indices** using a nearest-exact policy.

This avoids arbitrary fractional sample positions and improves the chance that independent requests hit the same integer samples.

A dense block is just a request whose requested sample count corresponds to every integer sample in the interval. Sparse UI sampling uses a much smaller count.

The API uses a half-open interval: `[begin, end)`. `SampleIndex` is an unsigned
64-bit global sample position. The planner owns the exact nearest-integer grid
mapping, so the node-facing request representation does not prematurely choose
an expansion or storage strategy.

### Compiled event requests are range queries, not sampling requests

Compiled event ports need a distinct request contract. Event data is sparse and
discrete; satisfying a `sample_count` by dropping events would change the
meaning of the stream. A compiled event query therefore requests a half-open
global interval and returns the events in that interval with their timestamps and
event values intact.

The required semantics are:

* querying `[begin, end)` returns every event in that interval;
* disconnected or out-of-extent regions return an empty event sequence rather
  than a fabricated event;
* event ordering is deterministic, including when multiple events share a
  timestamp;
* a compiled event input still behaves as an ordinary realtime event input for
  the current block, while additionally allowing range queries outside the
  current block; and
* any future UI aggregation/downsampling API is separate from compiled event
  port semantics and must not silently discard events.

The planner may still batch and union event intervals globally, but it must keep
event-range demand distinct from sampled scalar demand.

---

## 4. `tick_block()` versus `access_block()`

There are two distinct execution modes.

### `tick_block()`

Sequential realtime execution.

It:

* may access realtime and compiled inputs;
* may produce realtime and compiled outputs;
* may access the ordinary sequential `State`;
* may also use compiled-support state where useful;
* executes in the normal realtime graph order.

### `access_block()`

Arbitrary compiled-data evaluation.

It:

* sees only compiled inputs and compiled outputs;
* operates on arbitrary requested global sample positions/ranges for sample ports and global event intervals for event ports;
* cannot access realtime-only inputs or outputs;
* cannot access the sequential `State`;
* may access `CompiledState`;
* must produce results independent of the order in which access requests occur.

Do **not** provide fake silence for realtime inputs or discard sinks for realtime outputs in `AccessBlockContext`. Realtime ports should simply be unavailable there. This statically prevents an arbitrary-access implementation from accidentally depending on sequential realtime information.

The asymmetry is important:

> `tick_block()` may be implemented in terms of `access_block()`.
> `access_block()` must never be synthesized from `tick_block()`.

A stateful sequential `tick_block()` cannot generally be called out of order and therefore cannot satisfy arbitrary access.

---

## 5. Any compiled port requires an access implementation

If an `IV_NODE` declares at least one compiled port, its type must provide a valid compiled-access implementation.

This should be enforced at the `IV_NODE` declaration boundary with a targeted compile-time diagnostic rather than failing later in obscure trait machinery.

Conceptually:

```text
IV_NODE type 'Foo' declares compiled output 'bar'
but provides neither access_block(...) nor access_block_batch(...)
```

The same principle should apply to all static node constraints.

In particular, a registered node's unified `inputs()` and `outputs()`
declarations must be `static constexpr`. Their `InputConfig` / `OutputConfig`
variants carry both sample and event ports in authored order. This gives a
registered ID one immutable public port interface and leaves
configuration-dependent or variable-arity helpers as internal lowering nodes.
The compiler-inserted event fan-in/fan-out helpers follow that latter rule and
intentionally have no `IV_NODE` registration.

---

## 6. Unbatched and batched access callbacks

Do not call the simple version “scalar.” `access_block()` still operates on blocks/sparse sampling requests. Reserve “scalar” for actual sample-wise processing.

Authors may provide either:

```cpp
access_block(...)
```

or:

```cpp
access_block_batch(...)
```

but normally not both.

Internally, the traits layer identifies one unambiguous callback form and will
expose a batched operation to the execution layer.

If only `access_block()` exists, `access_block_batch()` is synthesized by invoking the unbatched implementation for every request in the batch.

So framework/compiler code will have one normalized entry point:

```text
traits::access_block_batch(...)
```

while simple nodes can implement the easier unbatched API.

The batched callback should see the requests for **all requested compiled outputs of the node at once**, not just several requests for one output. This permits sharing setup/work between outputs.

---

## 7. A compiled query is globally batched

The caller should be able to request data from many compiled output ports in one operation.

The obvious example is the UI: all currently visible compiled lanes should be submitted as one query.

The planner then merges all demands resulting from that query before executing anything.

For:

```text
      B
    ↗   ↘
A         C
    ↘   ↗
      D
```

if requests through both `B` and `D` eventually require data from `A`, `A` should not be called once through each path.

Instead:

```text
global output-request batch
        ↓
reverse demand planning
        ↓
requests through B and D accumulate at A
        ↓
union/coalesce A's requirements
        ↓
A executes once
```

This applies across all requested UI lanes, not one lane at a time.

A node should participate once in planning and once in execution for the entire global batch whenever possible.

---

## 8. Planning is separate from execution

Compiled access must **not** recursively execute upstream nodes greedily.

There are two phases.

### Phase 1: reverse demand planning

Walk involved nodes in reverse topological order.

Each node is told the complete accumulated request sets for its compiled outputs and reports what requests it requires from each compiled input.

Requirements reaching the same upstream port from multiple graph paths are unioned/coalesced before that upstream node is visited.

### Phase 2: forward evaluation

Once the complete demand graph is known, walk involved nodes in ordinary topological order and execute their batched compiled-access callbacks.

This retains the useful precomputed DAG ordering from realtime execution while preventing duplicate work.

The overall model is:

```text
batch of requested sink outputs
          ↓
reverse-topological requirement planning
          ↓
canonical union of requests per port
          ↓
choose temporary/intermediate representations
          ↓
forward-topological access execution
          ↓
return requested sink data
```

No persistent cache is required for this optimization.

Request coalescing and caching are distinct concepts.

---

## 9. Block-access propagation callback

A node therefore needs a way to describe:

> Given these requested samples on my outputs, what samples do I need from my inputs?

There should be unbatched/batched trait handling analogous to `access_block`:

```cpp
propagate_block_access(...)
propagate_block_access_batch(...)
```

with framework code always calling the normalized batched trait.

However, the context API should be extremely terse because dependency propagation is essentially the only purpose of this callback.

The desired form is approximately:

```cpp
void propagate_block_access_batch(auto& ctx)
{
    ctx.input<"x">(ctx.output<"y">());
}
```

`ctx.output<"y">()` directly returns the request-set object for that output.

`ctx.input<"x">(requests)` directly propagates/adds those requests to the specified input.

There should not be a multi-call protocol like:

```cpp
auto out = ctx.output<...>();
auto requests = out.requests();
auto in = ctx.input<...>();
in.require(requests);
```

unless additional functionality eventually makes that necessary.

For a transform with a larger dependency footprint:

```cpp
ctx.input<"audio">(
    expand_for_convolution(ctx.output<"out">(), ir_length)
);
```

For multiple outputs, the callback receives all of their already-unioned request sets together and can derive combined upstream requirements.

---

## 10. Request sets and unioning

Internally, each compiled port has a request set for the current global query,
but the request-set vocabulary depends on port kind.

A compiled **sample** request set must efficiently represent unions of dense and
sparse sampling requirements without eagerly expanding a sparse UI request into
millions of individual sample indices. It needs to support deterministic sampled
grids `(begin, end, count)`, dense ranges, unions, and overlap/coalescing while
retaining sparsity where useful.

A compiled **event** request set instead represents unions of event intervals.
Coalescing overlapping intervals is valid, but converting an event interval to a
sampled grid is not: the result still has to contain every event in the requested
interval.

The exact representations are intentionally implementation details.

For example, overlapping dense requirements may collapse:

```text
[0,512)
[256,768)
[400,900)

→ [0,900)
```

while a display request containing one sample every 10,000 samples should normally remain sparse.

---

## 11. Temporary materialization is decided after planning

Once planning has completed, the compiler/runtime knows all consumers and all requested regions for each intermediate value.

It can then choose efficient representations.

Examples:

```text
single consumer, fusion possible
→ no materialization

dense common range consumed repeatedly
→ one temporary contiguous array

sparse UI samples
→ sparse temporary result

tiny intermediate
→ stack/compiler arena

immutable resource
→ direct view/pointer

compiled producer consumed only sequentially
→ ordinary realtime connection lowering
```

A compiled input may therefore receive a contiguous pointer/span when that representation was chosen for the current query, but the graph semantics must not force every compiled connection into such a buffer.

Avoid the historical failure mode:

> “This connection might need random access, therefore always allocate and cache an entire buffer.”

---

## 12. Realtime → compiled connections are not implicit

The first compiled-port implementation does **not** support a realtime output
connected directly to a compiled input. There is no implicit recorder or
materialization edge.

If a graph needs that transition, it must use an explicit node with one
realtime input and one compiled output. That node owns recording semantics,
including how incoming samples are associated with global sample positions.

This keeps the initial graph semantics small and avoids committing the general
compiled-port model to a particular persistent recording implementation.

---

## 13. No compiled-output caching initially

The first implementation should deliberately have **no persistent cache of computed compiled sample/event outputs**.

Every query:

1. plans its dependencies;
2. computes the required data;
3. returns it;
4. may discard temporary computed results afterward.

Consequences:

* no `invalidate_spans()` API is needed;
* configuration changes require no cache invalidation;
* upstream data changes require no cache invalidation;
* automation edits simply affect the next query.

Persistent caching and invalidation should be designed together later, after actual workloads show where caching is useful.

The reverse requirement planner does not need to change when caching is eventually added: cached data can simply remove already-materialized samples from the newly planned demand.

---

## 14. `CompiledState`

`CompiledState` remains useful and is **not** the same thing as compiled port sample caching.

A node may declare:

```cpp
struct State {
    // sequential realtime state
};

struct CompiledState {
    // dynamic state useful to arbitrary-access computation
};
```

`CompiledState` can hold things such as:

* FFT plans;
* reusable workspaces;
* acceleration structures;
* node-managed memoization;
* preprocessed dynamic resources.

Its hard semantic constraint is:

> observable output must not depend on the order in which compiled access requests were made.

Different request orders may populate `CompiledState` differently for performance, but they must produce identical values.

`AccessBlockContext` may expose `CompiledState`, but never `State`.

`TickBlockContext` may need access to both when the realtime path benefits from compiled-side machinery. If both are exposed, use distinct accessor names rather than overloading an ambiguous `state()`.

---

## 15. Node lifecycle and `CompiledState`

Do not forget the existing node lifecycle API:

* `Node::declare()`
* `Node::initialize()`
* `Node::move()`
* `Node::release()`

All four need appropriate support for `CompiledState` as well as normal realtime `State`.

This should still describe lifetime/storage requirements, not dictate heap allocation.

The compiler remains free to:

* pack states in arenas;
* colocate states for cache locality;
* inline small states;
* omit absent states;
* share immutable precomputed objects where valid.

---

## 16. `tick_block_batch()`

The same trait-adapter pattern should be introduced for realtime block processing:

```cpp
tick_block(...)
tick_block_batch(...)
```

The framework always has a normalized batched operation. If only `tick_block()` is implemented, the batch trait loops over instances.

For realtime ticking, the batch dimension is **multiple compatible concrete node instances for the same realtime block**, not multiple time ranges.

Tiled nodes are the obvious immediate use case, but the concept should not be restricted to tiled nodes.

For example, a node that cannot SIMD effectively across the time dimension may be able to SIMD across eight homogeneous instances:

```text
sample 0: instances 0..7
sample 1: instances 0..7
...
```

A future whole-graph compiler may identify batching opportunities among unrelated instances sharing the same implementation.

Therefore `tick_block_batch()` should be designed around batches of compatible node instances, not around a special “tiled node” abstraction.

---

## 17. Automatic `tick_block()` from `access_block()`

The safe synthesis direction remains:

```text
access implementation
        ↓
possible realtime implementation
```

not the reverse.

If the node's realtime projection can be fully satisfied from its compiled-access implementation, traits may synthesize realtime ticking for the current block by requesting the current global range from the access implementation.

The static trait machinery must verify that doing so is semantically possible given the node's realtime-only ports.

There is no general rule that an arbitrary mixed node can get a synthesized `tick_block()`.

---

## 18. Static validation belongs at `IV_NODE`

The `InputConfig` / `OutputConfig` declaration model requires a registered
node's `inputs()` and `outputs()` to be `static constexpr`. That is part of the
port-declaration contract itself: a registered node ID has one immutable,
compile-time-visible ordered port interface, regardless of whether any future
port is realtime-only or compiled-capable.

When compiled declarations are introduced, `IV_NODE` should add the
compiled-specific validation needed by that capability on top of the existing
static declaration validation. For nodes whose compiled port contract is
statically described, validate at minimum:

* compiled ports require a valid access implementation;
* only one of unbatched/batched variants is user-defined for a given operation;
* callback signatures are valid;
* `tick_block_batch` / `tick_block` combinations are valid;
* `access_block_batch` / `access_block` combinations are valid;
* block-access propagation callback combinations are valid;
* `State`/`CompiledState` lifecycle functions are usable; and
* the compiled sample/event declarations are structurally valid.

The diagnostics should explicitly name the node type, offending callback/port,
and expected alternative whenever practical.

---

## 19. Example: automation clip

An automation clip is a representative compiled producer.

It owns control-point/configuration data and exposes a compiled smooth-curve output.

The UI can request:

```text
visible interval: [T0,T1]
display width: 1200 pixels

→ AccessRequest(T0, T1, 1200)
```

The node computes exactly the deterministic integer sample positions needed to draw the curve.

No full-resolution curve buffer is required.

If a downstream compiled transform consumes that curve, its block-access
propagation callback transforms/forwards its own output requests upstream. The
global planner sees all consumers and merges their demands before executing the
automation node once.

Editing a control point requires no invalidation machinery in the initial cacheless implementation. The next query simply recomputes its requested samples from the new control-point data.

---

## 20. Example: convolution

A convolution node demonstrates why requirements planning is necessary.

If its output receives a request for samples around some region, its
block-access propagation callback can state that it needs:

* an expanded region of the audio input;
* some finite extent of the IR compiled input.

The reverse planner aggregates this with requirements originating through any other downstream paths.

Only after all requirements are known does the runtime decide whether the audio/IR should be passed as direct views, temporary dense arrays, sparse results, etc.

An FFT implementation can use `CompiledState` for plans/workspaces without making the actual input/output samples persistently cached.

---

## 21. Architectural rules to preserve

The implementation should keep these principles explicit:

> **Sample/event kind and realtime/compiled capability are orthogonal. Compiled sample and compiled event ports are both first-class ordinary DSP ports; neither creates a parallel node graph.**

> **Compiled capability does not imply materialization.**

> **Random-access demand is propagated backward before computation runs forward.**

> **The whole query is planned as one batch, so converging DAG paths and multiple UI lanes can be coalesced before producers execute.**

> **Persistent computed-data caching is optional optimization infrastructure and is not part of the initial semantics.**

> **`CompiledState` is node-managed arbitrary-access state, not the framework's port-value cache.**

> **Nodes describe dependencies and computation; the compiler/runtime chooses storage, temporary layout, fusion, and caching strategy.**

That is the design point we reached. It should be enough to implement compiled DSP ports without carrying forward the old compiled-lane execution architecture or prematurely locking the future whole-graph compiler into a buffer-heavy representation.
