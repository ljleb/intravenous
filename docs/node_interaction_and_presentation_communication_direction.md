# Node Interaction And Presentation Communication Direction

_Status: normative direction for semantic backend-node events, coherent application
synchronization, and Vue-driven observation. Output-observation and range-query
semantics are specified in
[Vue Output Observation And Query Direction](./vue_output_observation_direction.md);
exact C++/TypeScript syntax and transport routes remain provisional until
exercised by the Vue client. The 2026-11-12 revision supersedes
backend presentation registration and the unimplemented `PresentationState` API;
see [the preserved earlier design](./historical/node_presentation_and_live_observation_before_vue_direction_2026-11-12.md).
The unified authored `State`/lifecycle direction of 2026-11-12 supersedes the
separate `TockState` authoring contract; see
[the preserved prior contract](./historical/node_state_lifecycle_before_unification_2026-11-12.md)._

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

- validating one explicit nested `State` type for a stateful concrete
  primitive, regardless of its Tick or Tock execution domain;
- rejecting aliases used as substitutes for those explicit state definitions;
- assigning stable field identities and recording offsets/type fingerprints;
- discovering typed node-event handlers;
- validating declaration/member-pointer use;
- generating serialization/deserialization code;
- emitting the language-neutral protocol information from which frontend-specific
  TypeScript bindings can be generated;
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

`declare()` describes the packed realtime realization. For Tick primitives,
`State` fields participating in declarations and compiler-owned realtime regions
contribute to `NodeLayout`. For Tock primitives the same authored `State` name
denotes background-owned state, not a realtime declaration or layout input.
No synthetic `DeclarationContext::tock_state()` is needed.

## One authored `State`; storage and lifecycle follow the primary callback family

A concrete primitive authors at most one nested `State` type, for either the Tick
callback family or the Tock callback family. The callback families are mutually
exclusive. The author does not choose storage through the state type's name:

- Tick `State` is part of the realtime realization and is packed into the
  canonical `NodeLayout`/`NodeStorage`, alongside compiler-selected audio-thread
  persistent data.
- Tock `State` belongs to separately owned background storage managed by
  `BackgroundExecutor`. It may contain dynamically allocated acceleration or
  memoization data and may resize as background coverage/content changes. It does
  not add to realtime `NodeLayout` or require realtime storage migration.
- Nodes with no `State` need no authored state allocation. Declaration-only/nested
  helper representations must be resolved in their owning declaration context;
  they must not be arbitrarily classified as executable Tick or Tock primitives.

The single authored type does **not** unify its physical storage, execution domain,
or correctness guarantees. Tock `State` is non-semantic acceleration: results,
coverage, and dependency requirements must remain correct if its contents are
discarded and freshly initialized, or if the order of background requests changes.
`initialize()` may establish configuration/resource-dependent acceleration; derived
input-content-dependent data is computed/revalidated by background evaluation.

### One authored lifecycle vocabulary, domain-specific capabilities

The authored lifecycle signatures are uniform regardless of execution family:

```cpp
void initialize(InitializationContext<MyNode> const& ctx) const;
void move(MoveContext<MyNode> const& ctx) const;
void release(ReleaseContext<MyNode> const& ctx) const;
```

Each context template determines the primary callback family statically. `ctx.state()`
is available for either family when `Node::State` exists; `MoveContext<Node>` also
provides `ctx.previous_state()` when relevant. Tick contexts retain the relevant
realtime `NodeStorage`/block/layout facilities. Tock contexts expose background-owned
state and relevant resources, **not** realtime `NodeStorage` or audio-thread-only
capabilities. A node author can change execution domain without renaming `State` or
rewriting unrelated lifecycle callback signatures; the callback body may still need
to change if it uses operations unavailable in the other domain.

`TockCoverageContext<Node>` and its Tock batch-lane views likewise expose
`ctx.state()` for background-owned state, rather than `ctx.tock_state()`.

### Tock generation migration is supported, but is not semantic persistence

Tock has the same optional authored `move(MoveContext<Node> const&)` operation as
Tick. Migration is considered for a compatible retained concrete node and state
representation at an ordered generation transition. It runs on the background
owner **after** predecessor evaluation using that state has completed; it must not
move or read a concurrently active predecessor. No audio-thread work or blocking
synchronization is introduced by background-state migration.

A node's `move()` is responsible for preserving or invalidating cached entries as
needed for changed configuration, inputs, coverage, and semantic version. If safe
migration cannot be established, fresh initialization is valid and required as the
fallback. An implicit state move may be used only where the state type and the
runtime's compatibility/lifetime contract make it safe; it is not a promise to move
all move-constructible Tock caches unconditionally. The executor must respect old
and successor ownership, construct/destroy each state exactly once, and run release
on the appropriate owner. Fresh initialization and reset remain correctness-preserving.

The **current implementation** still uses separate `TockState`,
`TockInitializationContext`/`TockReleaseContext`, and `TockStateStorage` records,
with fresh initialization and no Tock move callback. Those are migration targets,
not alternate intended authored APIs.

## Configuration, execution state, layout, and executable behavior are distinct

A semantic node has distinct aspects:

1. its construction/configuration value;
2. the contents of its domain-owned `State`, if any;
3. the realtime `NodeLayout` produced by a Tick realization's `declare()`;
4. its compiled Tick/Tock behavior and associated storage ownership.

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

`MyNode{8}` and `MyNode{32}` have the same Tick `State` type but different realtime
storage requirements. Conversely, configuration may alter executable behavior
without altering the resulting `NodeLayout`. Changes to background-owned
Tock `State` allocation do not by themselves alter `NodeLayout`.

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
and persistent-state semantics are compatible. Background-owned Tock `State`
lifecycle is managed separately, including optional safe migration.

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
use dynamic storage in its background-owned `State` and resize during
`tock*()` work without creating a new `NodeLayout` or forcing graph-wide
realtime-storage migration.

A useful statement of the intended division is:

> Configuration defines the realtime realization envelope for a Tick primitive;
> Tick `State` represents its realtime point within that envelope. A Tock
> primitive instead uses the same authored `State` name for reconstructible
> background acceleration outside the packed realtime realization.

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

A backend node cannot directly mutate another backend node's `State` (in
either execution domain), configuration, or realization.

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

The sending client receives the ordinary reliable request result, but that
reply alone does not synchronize other views or other clients. After acceptance,
`SocketRpcServer`'s revisioned session notifications deliver the committed
application-state change to subscribed clients (`session.subscribe`), with
`session.snapshot`/replay recovery when a revision is missed. This is the
existing **reliable semantic synchronization** path; it is not a live observation
frame and must not be implemented through the `Observations` module. The exact
shape of affected-node deltas versus refreshed read-model records is determined
by the first Vue read model, while preserving one coherent accepted result.

## Do not introduce `PresentationState` just to duplicate application data

`PresentationState` is not implemented. It is no longer a planned distinct C++
projection or a required generated frontend schema. Configuration, typed
construction arguments, identities, resolved ports and connections, manual-control
values, and mutation outcomes are authoritative application/graph information.
They should be exposed through the appropriate application-owned read model.

`State` retains its execution-domain-specific meaning as node execution state,
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
`State` fields from Tick nodes and output-port data. Exact observation hook syntax is deferred;
observations are **not** a permanent `node.state` projection of all implementation
storage. For a statically known definition, generated bindings may expose supported
field names and result types. The generic presentation can inspect output-port observation capabilities and
all input/output connection metadata without node-specific TypeScript source.

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

The normative design for observation operations, capability discovery, Tick
capture placement, Tock sparse/persisted output queries, the `Observations` app
module, and batched client interest/delivery is consolidated in
[Vue Output Observation And Query Direction](./vue_output_observation_direction.md).
It is driven by concrete Vue usage, not speculative RPC routes. Observations are
read-only with respect to graph semantics and never reuse semantic `on_message`
dispatch or require authored node `publish()`/subscription callbacks.

For the initial client, **only supported reflected Tick `Node::State` fields are
observable**. Tock `State` diagnostic observation is explicitly deferred; Tock
*outputs* remain observable through the existing background coverage and
persisted-data model. All inputs remain connectable but input-*value* observation
is deferred. Registered continuous and event types opt into operations whose
implementation respects the output's execution, access, and retention contract.

`Observations` is the single intended application module for reconciliation,
shared work, and batched transport results. `ProjectGraph`, `GraphJit`, and
`GraphExecutor` retain their graph-generation, compilation, and execution
ownership. The observation-specific ownership does not introduce a backend
presentation registry or an independent graph cutover coordinator.

# Default presentation

## Every node has a useful default presentation

The frontend always provides a generic Vue presentation, even if the node type has
no custom Vue component or observable runtime fields. It discovers and renders
**all available ports** for graph editing, including arbitrary port counts and
channels, with stable semantic endpoint identities. Ports stay connectable even
when their live visualizations are hidden at a given scale. The initial
generic presentation observes output *values*, not input values. The host can choose
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

The observation-specific unsettled implementation details and explicitly
excluded capabilities are recorded in
[Vue Output Observation And Query Direction](./vue_output_observation_direction.md).
Do not settle new RPC routes, public observation callback shapes, or compilation
representation until a concrete Vue feature exercises the full path.

The exact transaction/journal representation for ordered root semantic events,
partial/successor `NodeStorage` migration, and frontend regeneration of typed
semantic event bindings also remain separate implementation decisions.
