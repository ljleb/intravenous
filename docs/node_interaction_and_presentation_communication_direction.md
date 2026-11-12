# Node Interaction And Presentation Communication Direction

_Status: normative direction for semantic backend-node events, coherent application
synchronization, and Vue-driven live observation. The observation operation
contract is intentional; its exact C++/TypeScript syntax and transport routes remain
provisional until exercised by the Vue client. The 2026-11-12 revision supersedes
backend presentation registration and the unimplemented `PresentationState` API;
see [the preserved earlier design](./historical/node_presentation_and_live_observation_before_vue_direction_2026-11-12.md)._

This note is about communication with **semantic backend nodes** and their
presentations. It is not a description of the current VS Code extension transport
implementation. JSON-RPC/webview/socket details are transport adapters below this
model and must not leak into node or presentation APIs.

Related documents:

- [Node Presentation And Manual Controls Direction](./node_presentation_and_manual_controls_direction.md)
  covers presentation display modes, interaction lifetime, and manual-control/JIT
  specialization concerns that remain separate from this communication model.
- [Node-Reference DSL Contract](./node_ref_dsl_contract.md) describes the current
  builder-facing `NodeRef` contract and the planned ownership migration.
- [Event Propagation Tree Constraint](./event_propagation_tree_constraint.md)
  describes the analogous tree constraint between application modules. The node
  interaction tree in this document is a distinct lower-level mechanism.
- [IV Module Source Introspection Direction](./iv_module_source_introspection_direction.md)
  describes the current source-introspection read model. The compiler/plugin
  responsibilities described here are broader than that application read model.

## Architectural split

The current direction separates three mechanisms that one user action may involve:

```text
typed semantic node interaction
    reliable UI -> backend root event
    ordered one-way backend node -> node propagation

realization change
    reconfiguration / recompilation / possible NodeLayout change
    safe publication and storage migration at a pass boundary

presentation synchronization
    authoritative semantic/application data after committed interactions
    separately, disposable, selectively captured runtime observations
```

These mechanisms cooperate, but none is a substitute for another.

## Source introspection is part of the IV language contract

The Clang/source-introspection plugin should be mandatory infrastructure for IV
packages rather than optional metadata generation. It defines and validates the
IV-specific language rules that ordinary runtime/library code may then assume.

Its authoritative responsibilities may include:

- validating the restrictions on explicit nested `State` and `TockState` types;
- rejecting aliases used as substitutes for those explicit state definitions;
- assigning stable field identities and recording offsets/type fingerprints;
- discovering typed node-event handlers;
- validating declaration/member-pointer use;
- generating serialization/deserialization code;
- generating the frontend TypeScript protocol surface;
- providing stable semantic identities required by graph and presentation tooling.

The runtime should not defensively rediscover source facts already established by
this compilation layer.

### `declare()` describes realtime storage, not a fake state object

The declaration API should move toward member-pointer template syntax:

```cpp
ctx.local_array<&State::values>(count);
ctx.import_array<&State::input>("input");
ctx.export_array<&State::output>("output");
```

rather than requiring a synthetic `State` object only so declaration code can
recover field offsets. Once source introspection makes the member relationship
authoritative, `DeclarationContext::state()` should disappear.

`declare()` describes the packed realtime realization and therefore declarations
associated with `State` and compiler-owned audio-thread storage. `TockState` is not
part of this declaration/layout model and should not need a synthetic
`DeclarationContext::tock_state()` either.

## `State`, `TockState`, and `NodeStorage` have different ownership

`NodeStorage` exists to make the audio-thread realization as cheap to execute as
possible. It is the fixed-layout packed storage addressed by the JIT: authored
`State` plus compiler-selected audio-thread data that must survive execution calls.
Its layout should be chosen for compactness/locality and, where useful, roughly the
order in which generated audio-thread code will touch it.

`TockState` is different. It is background-thread state used by Tock/background
work and must not be packed into `NodeStorage`. Ordinary dynamic allocation is
allowed inside `TockState`; its size and internal allocations may change as
background work changes. This is particularly important for acceleration structures
whose useful size depends on actual input coverage/content rather than on the
compiled realtime realization.

A node may still set up its instance's `TockState` from `initialize()`. For example,
`initialize()` may allocate configuration/resource-dependent tables or establish
background acceleration state that is meaningful before any particular input
contents are examined. Input-dependent derived contents, however, must be computed
or recomputed by the appropriate background `tock*()` work rather than by
`initialize()`.

`TockState` remains non-semantic acceleration/memoization state: observable results
must not depend on its history, and correctness must survive discarding and freshly
reinitializing it. Whether an implementation opportunistically retains/moves a
compatible `TockState` across realization changes is a background-lifecycle policy,
not part of `NodeStorage` compatibility.

## Configuration, realtime state, background state, layout, and executable behavior are distinct

A semantic node has at least five independent aspects:

1. its construction/configuration value;
2. the contents of its realtime `State`;
3. its background-only `TockState`;
4. the realtime `NodeLayout` produced by `declare()`;
5. its compiled Tick/Tock behavior.

The same C++ `State` type can produce different realtime storage layouts:

```cpp
class MyNode {
    int how_much;

    struct State {
        std::span<int> ints;
    };

    void declare(auto& ctx) const {
        ctx.local_array<&State::ints>(how_much);
    }
};
```

`MyNode{8}` and `MyNode{32}` have the same `State` type but different realtime
storage requirements. Conversely, configuration may alter executable behavior
without altering the resulting `NodeLayout`. Changes to `TockState` allocation do
not by themselves alter `NodeLayout`.

The intended realtime-storage rule is therefore:

```text
same resulting NodeLayout
    -> retain the existing NodeStorage

different resulting NodeLayout
    -> prepare a complete successor packed NodeStorage
       migrate/publish at a safe pass boundary
```

Executable replacement is independent of realtime-storage replacement. A new
compiled realization may reuse the existing `NodeStorage` when its realtime layout
and persistent-state semantics are compatible. Background `TockState` lifecycle is
managed separately.

Nodes themselves decide how structural a realtime parameter is. A node may
deliberately bucket/overallocate its declaration:

```cpp
auto capacity = round_up(logical_size, 16);
ctx.local_array<&State::values>(capacity);
```

so changes inside one capacity envelope do not force a new layout. Exact sizing,
fixed multiples, geometric growth, hysteresis, reluctant shrinking, or other
policies belong to the node implementation rather than `NodeStorage`.

By contrast, background data whose useful size follows input coverage/content can
use dynamic storage in `TockState` and resize during `tock*()` work without creating
a new `NodeLayout` or forcing a graph-wide realtime-storage migration.

A useful statement of the intended division is:

> Configuration defines the realtime realization envelope; `State` represents the
> current realtime point within that envelope; `TockState` is reconstructible
> background acceleration state outside the packed realtime realization.

# Semantic node events

## Events represent relatively discontinuous semantic changes

The node-event subsystem is for reliable semantic operations: configuration and
structural changes, assignments/transfers, operations that affect several nodes,
and other changes where dropping the request would be incorrect.

It is not the backend-to-UI telemetry/live-observation mechanism described later.
High-frequency UI requests may eventually be coalesced or superseded before
application, but that is distinct from using an unreliable transport where the
latest intended mutation may disappear.

## Leaf-node handlers

A leaf node can receive a typed event directly through this public member shape:

```cpp
void on_message(
    NodeMessageContext<MyNode>& ctx,
    SomeEvent const& event);
```

The semantic contract is:

- dispatch is by the C++ event type;
- the handler is associated with the receiving backend node;
- the handler may mutate/reconfigure/replace only that node;
- a replacement supplies the successor definition and its complete typed argument
  list;
- the leaf-node handler reads values that it preserves through `this`;
- structured, list, and container arguments remain first-class successor arguments;
- the handler may send further typed events to semantic node targets;
- the public context exposes no `NodeStorage`, concrete node indices, graph
  generations, RPC details, sockets, or editor/webview plumbing;
- `ctx.send(...)` is one-way and has no synchronous result.

## Module-node handlers

A module node is simultaneously a semantic backend node that can be presented and
an authored scope containing descendants. A module that receives semantic node
messages is a class registered with `IV_MODULE_CLASS`. Its constructor receives the
registered construction arguments. Its `build_graph(GraphBuilder&) const` member
creates the authored graph. Public, const `on_message` overloads receive typed events:

```cpp
class FilterVoice {
public:
    explicit FilterVoice(float cutoff) : cutoff_(cutoff) {}

    void build_graph(GraphBuilder& g) const
    {
        auto filter = g.node<"filter">(cutoff_);
        auto envelope = g.node<"envelope">();
        filter("cutoff"_P = envelope);
        g.outputs("out"_P = filter);
    }

    void on_message(
        ModuleMessageContext<FilterVoice>& ctx,
        ResetEnvelope const& event) const
    {
        ctx.send(ctx.ref<"envelope">(), event);
    }

private:
    float cutoff_;
};

IV_MODULE_CLASS("iv.example.filter_voice", FilterVoice);
```

`ModuleMessageContext<Module>` is a separate public type from
`NodeMessageContext<Node>`. `ctx.ref<"name">()` accepts a compile-time string and
selects a graph-significant local declared with that name in `build_graph()`. The
compiler records the local's `ProjectVirtualNodeSelector`. The returned
`ModuleMessageTarget` can append `ProjectVirtualMemberSelector` with `.member(n)` or
`ProjectTiledChildSelector` with `.tiled_child<"name">()`. It does not expose a
`NodeBundleHandle` or retain a `NodeRef` after graph construction.

A module handler may coordinate descendants only by sending them events. It reads
construction values from the retained class instance through `this`. A function
registered with `IV_MODULE` remains the shorter form for a module with no message
handlers.

The replacement call supplies a complete typed successor argument list. Package
code converts that list to one ordered JSON argument array before the handler returns.
Generated `ConfigurationExpressionTypeSchema` operations read scalar values,
aggregate fields, and sequence elements. The node author does not provide a separate
JSON serializer for ordinary Front 9 values. The application does not inspect
package-owned C++ fields.

The same value model handles list and container arguments used by module automation.
Front 9 owns this structured configuration value model. See
[Configuration Expression Language Direction](./configuration_expression_language_direction.md).

The intended replacement shape is:

```cpp
ctx.replace<"iv.example.successor">(
    successor_argument_1,
    successor_argument_2);
```

For a leaf node, the preserved values are ordinary members available through `this`.
For a class module, the preserved values are ordinary members available through
`this`.

A successful replacement is the new semantic node. It is not a temporary modifier
applied to one `ConfiguredGraph`. The application stores the successor definition and
its complete structured configuration under the node's stable `NodeAddress`, reapplies
that replacement whenever `NodeInstances` reconstructs the project graph, and includes
it in project save/load. Later graph edits and package rebuilds therefore reconstruct
the replacement rather than silently restoring the node originally authored by the
enclosing module. A subsequent replacement replaces this complete record; it does not
accumulate an unordered stack of modifiers.

## Event types are ordinary reusable C++ types

An event does not have to be nested in or uniquely owned by the receiving node:

```cpp
namespace audio_clip {
    struct ConsumeFrom {
        NodeAddress source;
    };
}
```

Several unrelated node types may handle the same event. Supporting an event is
structural: a node supports the protocol because it has the corresponding typed
handler.

A separate nominal protocol declaration system is not required by the current
design.

## Nodes own their state exclusively

A backend node cannot directly mutate another backend node's `State`, `TockState`,
configuration, or realization.

If node A requires node B to change, A sends B an event. This is a hard semantic
boundary, not merely a preferred coding style.

Therefore event propagation records the complete causal path for cross-node
semantic mutation.

## One root interaction is an ordered graph interaction

One presentation-originated root event may cause a tree of backend node events.
Operations performed by event contexts have program-order semantics and cannot be
reordered into unrelated buckets such as "all writes", "all replacements", and
"all sends".

For example, a handler may intentionally require:

```text
resize destination
transfer into destination
shrink source
```

or:

```text
replace a semantic node
then send/address relative to the successor semantic world
```

The implementation should therefore behave like an ordered interaction
journal/program even if expensive preparation is performed away from the realtime
thread.

Exactly how that ordered program is prepared, rebased, and committed across a
pass boundary remains an implementation problem rather than a settled API.

## One delivery per concrete node per root interaction

Within one root interaction, each concrete node may enter an event handler at most
once. A second delivery to the same concrete node is an interaction error.

This excludes cycles and convergent diamonds such as:

```text
A -> B -> D
A -> C -> D
```

for one root cause. Selectors may expand to many concrete nodes, but each concrete
member may receive the event only once. Overlapping target sets that would deliver
twice are diagnosed.

The rule also encourages logical batching:

The following type illustrates only the shape of one event payload. It does not
define a node constructor or say that `attack` and `release` should be construction
arguments. A node whose envelope parameters are continuous controls should normally
expose them as input ports.

```cpp
struct ConfigureEnvelope {
    float attack;
    float release;
};
```

rather than several independent setter events that are really one operation.

## Backend event data flow equals backend event control flow

There is no synchronous request/return edge between backend nodes:

```cpp
ctx.send(target, event); // no returned node result
```

Information required by a downstream handler travels in the event payload on the
same edge that transfers control. For this subsystem, the propagation tree is
therefore both the control-flow tree and the cross-node data-flow tree.

Synchronous intra-graph queries/return values should not be introduced without a
concrete use case that justifies the extra ordering and migration semantics.

## Unsupported dynamic targets are diagnostic rather than catastrophic

When static types make incompatibility knowable, source-introspection/compiler
tooling should diagnose an unsupported event as early as practical.

Dynamic selectors may resolve to heterogeneous concrete nodes. A dynamically
selected target that does not implement the event should normally produce a
diagnostic/warning and be skipped rather than crashing the application.

## Authored graph edits can be root-node events

The user-authored graph has a semantic root/module node corresponding to the
project/root scope containing authored nodes and arbitrary authored extra
connections between them.

Operations such as:

```text
create/delete an authored node
connect/disconnect authored ports
change root/module graph structure
```

can therefore be modeled as semantic events addressed to that root/module node,
rather than requiring an unrelated application-mutation protocol. The root handler
coordinates descendants through the same event/graph-building semantics available
to module nodes.

# Semantic node identity

## `NodeRef`, `NodeAddress`, selectors, and concrete nodes have different roles

A `NodeRef` is the authored C++ reference. In addition to its builder-time role,
the planned event model requires it to carry/read the stable virtual/semantic node
identity needed by a captured module handler.

A `NodeAddress` is the serializable form of semantic identity that may cross the
presentation/backend boundary or appear inside ordinary event values.

A selector is a semantic query rooted at one of those identities. Selectors always
represent sets, possibly empty. A virtual node may resolve to several concrete
nodes.

A concrete node/index is realization-specific runtime detail and is never a
presentation-facing identity.

Direct dispatch should therefore not require pointless selector construction:

```cpp
ctx.send(node_ref, event);
ctx.send(node_address, event);
ctx.send(selection, event);
```

all mean "dispatch to the concrete set represented by this semantic target".
`ctx.select(...)` is needed only when further traversal/filtering is desired.

## Semantic identity crosses the UI boundary; backend state does not

For interactions such as dragging the semantic identity of one node onto another,
the frontend may send a `NodeAddress` in an event payload. The receiving backend
node then coordinates the actual state/data transfer entirely on the backend.

Large or implementation-specific backend state should not travel through Vue as
an intermediary merely because the interaction began in the UI.

# Coherent semantic synchronization without a separate `PresentationState`

## Commit authoritative information after the root interaction

A successful semantic node event and any resulting realization change must have a
coherent, authoritative result visible to all presentations of the same semantic
node. Presentations must not observe intermediate mutation results while the root
event is still propagating. The intended order remains:

```text
root UI event
    -> complete ordered backend node-event propagation
    -> determine accepted semantic/realization result
    -> publish coherent affected application/graph data
```

Ordinary node-event handlers should not need `ctx.reply()` or `ctx.notify()` to
keep Vue consistent. The event tree's affected nodes and structurally changed
semantic descendants can inform the invalidation/synchronization set. The server
publishes committed results from the owning application structures rather than
requiring nodes to manually send presentation messages.

## Do not introduce `PresentationState` just to duplicate application data

`PresentationState` is not implemented. It is no longer a planned distinct C++
projection or a required generated frontend schema. Configuration, typed
construction arguments, identities, resolved ports and connections, manual-control
values, and mutation outcomes are authoritative application/graph information.
They should be exposed through the appropriate application-owned read model.

`State` and `TockState` retain their existing meanings as node execution state,
not semantic configuration or automatically authoritative UI state. A presentation
may separately observe supported reflected runtime fields as *disposable live
information*. A Vue component may also own local display and interaction state.
These three categories do not become interchangeable merely because they appear
inside one component.

If future node-event semantics cannot be reconstructed from the existing
application's committed data, that concrete gap may justify another projection.
It does not justify declaring one prematurely. The superseded `PresentationState`
contract and its rationale are preserved in
[historical documentation](./historical/node_presentation_and_live_observation_before_vue_direction_2026-11-12.md).

# Vue presentation API

## Vue holds a semantic `node` object

The presentation-facing abstraction is independent of the transport adapter. A
specialized Vue component may obtain a typed semantic `node` (conceptually through
`useNode<MyNode>()`); a generic component may obtain a dynamically typed one.
Its stable capabilities include the conceptual equivalents of:

```ts
node.address    // stable semantic NodeAddress
node.ports      // dynamic, resolved input/output descriptors
node.send(...)  // reliable, typed semantic node event
```

A Vue presentation may also request observation of selected, supported reflected
`State`/`TockState` fields and port data. Exact observation hook syntax is deferred;
observations are **not** a permanent `node.state` projection of all implementation
storage. For a statically known definition, generated bindings may expose supported
field names and result types. The generic presentation can inspect ports and their
available operations without any node-specific TypeScript source.

Vue must not see concrete node indices, `NodeStorage` offsets, `NodeCodeKey`,
compiled realization objects, or socket/JSON-RPC method details. A semantic
`NodeAddress` survives compatible graph revisions; individual observation capture
bindings remain generation-specific.

## Events are strongly typed in frontend bindings

A language-specific frontend binding must not degrade to a string event name plus an
untyped/raw JSON object such as:

```ts
// not the intended API
node.send("SetCutoff", { cutoff: 800 })
```

The backend compiler discovers the authoritative C++ event definitions and emits a
language-neutral schema. A TypeScript/Vue adapter can generate concrete event values,
types, codecs, and factories from that schema. A future C++/Vulkan frontend can consume
the same schema without retaining a dependency on TypeScript. `node.send(...)` in each
frontend accepts that frontend's typed values. For example, one possible TypeScript
adapter could expose:

```ts
const event: SetCutoff = { cutoff: 800 }
node.send(event)
```

or an equivalently strongly typed generated constructor/API.

JSON is the current transport representation, not the programming model. The adapter
generates or implements the JSON codecs from the neutral schema. Vue authors should
neither select event types with raw strings nor manually serialize their payload
objects. The same rule applies to authors of another frontend.

The same principle applies to generated schemas for supported observations; no
separate generated `PresentationState` schema is required.

## The backend publishes a language-neutral event protocol

The mandatory source-introspection/compiler layer emits a
`<package>.ivpkg.bc.node-events.json` sidecar for each finalized package. This artifact
contains the stable nominal ID, definition fingerprint, diagnostic display name, and
JSON payload structure of every event used by a discovered handler. It also maps each
registered leaf or module definition ID to its supported event types. An unregistered
inner leaf type has no stable public definition ID and therefore appears only through
the runtime address catalog. The sidecar permits offline binding generation without
loading the package into a running server.

The runtime method `nodeMessages.getProtocol` returns a coherent snapshot containing:

- `formatVersion`;
- `projectGraphGeneration` and `definitionsGeneration`;
- the event schemas retained by the accepted package revisions;
- registered leaf/module definition IDs and their supported event types;
- each active `NodeAddress` and its `supportedEventTypes`.

Event classes remain independent C++ types. They are not nested in leaf-node or
module-node types, and several node types can reuse the same event identity and schema.
The active-address list represents that many-to-many relationship directly.

The backend owns C++ event discovery, the neutral schema, payload validation and
deserialization, active-address compatibility, dispatch, and generation identity. A
frontend adapter or supplementary plugin owns language-specific types, codecs,
factories, naming/collision rules, imports, and GUI integration. The backend must not
contain TypeScript or Vue generation rules.

The neutral protocol must not expose state-field offsets, packed `NodeStorage` layout,
`NodeCodeKey`, LLVM symbols, function pointers, concrete node indices, migration
metadata, or compiled realization details. Source introspection may know those ABI
facts, but it emits them only to backend artifacts. The protocol uses nominal ID plus
definition fingerprint for compatibility; `displayName` is diagnostic only.

`convertedScalar` is the one intentionally provider-validated payload kind. Its schema
lists the JSON scalar forms that can reach the provider constructor and marks
`providerValidated: true`; the exact C++ constructor can reject a listed form. A binding
generator must preserve that validation boundary instead of claiming that every listed
form constructs every provider-defined scalar. Primitive numeric, Boolean, string,
enumeration, aggregate, sequence, and `NodeAddress` schemas remain structurally exact.

## Static event compatibility should be preserved where possible

Because source introspection discovers typed handlers, a language-specific adapter for
a statically known presentation/node type should expose an event-send surface that
rejects unsupported events at that language's compile time where practical.

Likewise, statically known C++ `NodeRef` targets can be diagnosed when an event is
known not to be supported. Runtime diagnostics remain necessary for dynamic
selectors whose concrete target set cannot be known statically.

# Live backend-to-presentation information

## Separate requirements, capture operations, and delivery

The intended live-observation system is driven by real Vue presentation needs, not
by speculative RPC routes. A presentation asks for selected observations of a
semantic node's reflected fields or resolved ports. A registered type provides the
finite set of observation operations meaningful for that source. GraphJit plans
only the required executable captures; the application collects and batches their
results. Concrete routes, public C++ signatures, and Vue binding syntax should be
settled alongside the first functioning Vue presentation.

An observation is not a semantic node mutation: missing intermediate *replaceable*
snapshots is acceptable, and the reliable node-event/application revision stream
remains separate. An observation is never an instruction for a node to send a
socket notification. No default `ctx.publish()`, `on_message`, `on_subscribe`, or
`on_subscription_change` callback is required on authored leaf or module nodes.

## Discovery from existing state reflection and resolved ports

Use existing `NodeStateStructure`/`NodeStateFieldStructure` reflection for `State`
(and the appropriate source metadata for `TockState`) together with resolved port
metadata. Discovery names a field or port and its type; **observability** additionally
requires a registered, safe capture/transport operation for that type and execution
context. A discoverable but unsupported field is not an observation error or an
implicit byte-serialization request. It simply advertises no usable observation
operation. Do not expose arbitrary pointers or runtime storage-layout details to
Vue.

Initially support a fixed set of scalar/state-field types through a global
registered type codec/capture system. Generic implementations can be reused by
several registered types, with each type explicitly opting in and supplying its
type-dependent behavior. Unsupported types remain unobservable. Per-field codec or
serialization overrides and arbitrary custom node-authored producers are deferred.
The source of a field need not be modified just to add an ordinary observation.

For continuous ports, extend the existing closed registered continuous-value type
system with **explicitly supported observation operations**. Reusable operation
implementations may be templated; each value-type extension chooses the operations
that actually make sense for its value shape, dimensions, channels, and realization.
The existence of an operation for `Sample` must not imply its existence for
`FFTBlock`, `FractionalGlobalIndex`, or a future value type. Source access and
retention semantics further constrain availability: Sequential/Tick/ephemeral
outputs are not interchangeable with persisted or random-access data.

## Vue chooses the observation operation reactively

A generic Vue presentation discovers all of a node's input/output ports so users can
connect them. It may visualize only the relevant subset for its mode and geometry.
A specialized presentation may refer to supported reflected fields by generated
static names and types; a generic component discovers supported port operations at
runtime. Different presentations may request different operations for the same
source, and the same presentation may revise them when scale, mode, visibility, or
interaction state changes.

For example, one `Sample` output might offer the distinct operations `latest`,
`level`, and `waveform` (not necessarily all in the first implementation):

| Operation | Intended result | Capture behavior |
| --- | --- | --- |
| `latest` | A replaceable instantaneous value or bounded current block | Snapshot while valid; suppress unchanged results where meaningful |
| `level` | A low-cost sample-level measure for a meter | Optional interval reduction (e.g. sample peak and/or RMS) |
| `waveform` | A bounded recent time-domain window | Capture samples or a useful min/max envelope; reduce for display resolution |

These are **type-registered alternatives**, not a universal enum applied to every
port. `level` does not turn every `Sample` observation into a peak detector;
`waveform` does not require permanent raw-sample history when unrequested. A future
`FFTBlock` observation may expose spectrum data using its own type-specific shape.
Exact initial operation IDs and parameters follow concrete Vue usage.

The frontend host aggregates computed observation requirements; individual Vue
component mount/unmount events must not become immediate individual RPC requests.
The host can register the desired requirement set when a node approaches the
viewport and recalculate it reactively when the component's inputs change. A full
set replacement or equivalent reconciled batched delta should establish the
server's intended interest state.

## GraphJit places minimal, optional observation work

Capture points must be located where the observed field or port value is valid.
They cannot all execute at the end of the root Tick callback: ephemeral outputs
may have been overwritten, native region execution may have hidden intermediate
values, and some values require a producer or post-operation boundary. `State` and
`TockState` also have different thread ownership. Non-realtime code must not read
mutable realtime storage concurrently without a safe snapshot/handoff.

A registered observation operation should supply the equivalent of:

1. **Describe:** supported source type/shape, parameters, and result schema.
2. **Prepare:** select a legal capture point and preallocate any bounded storage.
3. **Capture:** perform bounded, allocation-free, nonblocking work at the correct
   execution point, including realtime when necessary.
4. **Collect:** transfer or read a coherent observation-owned result off realtime,
   perform type-aware comparison/reduction/serialization, and produce a transportable
   value when changed.

These are required capabilities of an operation, not necessarily four public virtual
callbacks. A cheap `latest` snapshot may use a generic implementation; a waveform
or level reduction may use type-owned code. GraphJit should consume normalized
operations instead of switching centrally on known visualization kinds. Type
extensions can share generic templates while registering only valid operations.

The union of currently required observations across clients is an input to
GraphJit's incremental realization. Adding/removing an observation may replace an
immutable capture/root plan at a safe execution boundary. Preserve existing node
instances, `NodeStorage`, `NodeLayout`, scratch allocation, scheduling, and native
specializations whenever the observation can be attached without changing them.
Observation-only changes should do the minimum actual compile/replan work; there is
no blanket rule forbidding recompilation when the viewport changes.

Some observations genuinely affect execution: an otherwise unused output may need
production or materialization, and some probes may require storage or different
specialization boundaries. Change those parts **only when necessary** for the
requested observation, instead of pretending every source can be captured with a
layout-neutral end-of-root read. Activating/deactivating a probe should otherwise
remove its unused realtime work, not leave checks on every node indefinitely.
Viewport overscan, interest-set hysteresis, and deferred deactivation should avoid
frequent recompilations during small or rapid movements.

## Batching, shared production, and subscription lifetime

Batch in both directions:

- Vue sends reconciled sets of desired observation operations/parameters for each
  client-owned presentation scope, rather than one RPC per node/component event.
- The server merges compatible interests across all clients and only changes the
  effective producer/capture plan when the combined requirements change.
- The observation service collects changed results and emits bounded,
  presentation-rate **batches** per client. It does not deliver one socket message
  per node or enqueue one reliable project-revision notification per meter block.

Use replaceable/coalesced data for state-like observations. An unchanged snapshot
should not require a new client update. Update rates are maximum publication rates,
not mandatory periodic traffic. Do not sacrifice significant interval information
when the requested operation's semantics require accumulation: a `level` operation
can retain the maximum sample magnitude over the collection interval, while
`waveform` retains an appropriate bounded window. This is **sample peak**, not
costlier inter-sample *true peak* detection. True peak or other expensive DSP may
be an explicit node/operation, never a requirement of every generic meter. Event
histories requiring every occurrence are not silently converted into replaceable
snapshots.

Use three interest tiers as a host policy: **visible** (full requested quality),
**nearby/predicted** (cheaper prefetch or lower cadence), and **inactive** (no
observation work, apart from a bounded shutdown grace period). Overscan, viewport
hysteresis, and predictive prefetch prevent rapid interest churn while panning or
zooming. Tier changes need not rebuild GraphJit if only the publication cadence or
frontend reduction changes; different capture operations or resource needs may.

Subscription correctness must not depend on every Vue component explicitly
unsubscribing. An authoritative per-client/per-scope desired set, connection-disconnect
cleanup, scoped lease expiry/reconciliation, and graph-generation rebind/invalidation
bound interest lifetime. Explicit unsubscribe may release resources sooner but is
not required for correctness. Do not keep pointers to reclaimed execution
generations. The server validates all source identities and parameters, applies
resource/bandwidth limits, and coalesces replaceable frames for slow clients rather
than filling the reliable replay log or disconnecting them for obsolete telemetry.

The application-module ownership of the observation registry and batching is not
fixed yet. A presentation/observation owner such as the prospective
`PresentationInstances` may collect results and hand frames to `SocketRpcServer`,
without introducing a dedicated thread unless actual scheduling/concurrency needs
justify one. It must respect the application event-tree constraint: an asynchronous
outgoing batch is a distinct cause, not re-entry into the incoming request's tree.

# Default presentation

## Every node has a useful default presentation

The frontend always provides a generic Vue presentation, even if the node type has
no custom Vue component or observable runtime fields. It discovers and renders
**all available ports** for graph editing, including arbitrary port counts and
channels, with stable semantic endpoint identities. Ports stay connectable even
when their live visualizations are hidden at a given scale. The host can choose
which observations to activate from registered capabilities rather than requiring
authored nodes to forward port values.

For `nano`, `mini`, `compact`, and `editable` modes, choose representations that
fit the geometry and source capabilities. Examples include an activity indication,
scalar numeric value, sample-level meter, waveform, or persisted-data view. Do not
require all ports to have a waveform or meter, and do not special-case only the
first input and first output as the generic representation of a node. The actual
visualization policy should be tested in Vue rather than frozen as a universal
port-observation rule.

# Deferred implementation choices

The direction intentionally does **not** settle:

- the first Vue observation hook signature or any new RPC route, before a real
  component needs it;
- the exact C++ registration template/descriptor layout for type observation
  operations and the exact first set of transport-supported field types;
- field-specific serialization overrides, arbitrary user-defined producers, or
  observation-lifecycle callbacks on authored nodes;
- whether a given observation-plan change is represented as a separate immutable
  overlay or an updated `RealtimeRootPlan`, and which native-region boundaries
  can be probed with no specialization change;
- detailed persisted/random-access observation/query integration, buffer sizing,
  and publication cadence, including frontend-driven resolution changes;
- the TypeScript code-generation/tooling workflow for observing optional fields
  and operation-specific result types;
- the exact transaction/journal representation for ordered root semantic events,
  partial/successor `NodeStorage` migration, and frontend regeneration of typed
  event bindings.

Begin the Vue presentation host and use the first reflected scalar-field
observation plus a real `Sample` output observation to drive the smallest complete
end-to-end implementation. Measure observation-only incremental compile latency,
realtime overhead, specialization effects, and subscription churn before creating
new node-facing hooks or a general-purpose speculative transport API.
