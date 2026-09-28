# Node Interaction And Presentation Communication Direction

_Status: design direction for the backend-node event model and presentation
communication contract. The semantic contracts in the first part of this note are
considered settled enough to guide implementation even where exact API names are
still provisional. The live-observation section intentionally records constraints
without standardizing an API that the application requirements do not yet justify._

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
    authoritative PresentationState after semantic interactions
    separately, disposable demand-driven live observations
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

A leaf node can receive a typed event directly. The exact API names remain
provisional, but the intended shape is:

```cpp
void on_message(
    NodeMessageContext<MyNode>& ctx,
    SomeEvent const& event);
```

The semantic contract is:

- dispatch is by the C++ event type;
- the handler is associated with the receiving backend node;
- the handler may mutate/reconfigure/replace only that node;
- when replacing/reconfiguring itself, the handler can inspect the predecessor's
  current configured argument values and preserve all arguments except the selected
  values/subvalues it intentionally changes;
- structured/list/container arguments remain first-class values so a node or module can
  automate an evolving construction-argument set without rebuilding it from unrelated
  callback captures;
- the handler may send further typed events to semantic node targets;
- the public context exposes no `NodeStorage`, concrete node indices, graph
  generations, RPC details, sockets, or editor/webview plumbing;
- `ctx.send(...)` is one-way and has no synchronous result.

## Module-node handlers

A module node is simultaneously a semantic backend node that can be presented and
an authored scope containing descendants. It needs the equivalent typed-handler
capability, registered while constructing that scope. The exact syntax remains
provisional, but the intended capability is represented by:

```cpp
auto filter = g.node<"filter">(...);
auto envelope = g.node<"envelope">(...);

g.on_message(
    [filter, envelope](ModuleMessageContext& ctx,
                       SomeEvent const& event) {
        ctx.send(filter, ...);
        ctx.send(envelope, ...);
    });
```

A module handler may coordinate its descendants only by sending them events. It
should be able to capture authored `NodeRef`s that are already in lexical scope;
a module author should not have to reconstruct those identities through string
selectors merely because dispatch happens later at runtime.

That requires the captured reference to contain/read a stable semantic identity
rather than dereferencing a stale builder/session pointer when the handler runs.

Captured values remain useful for ordinary handler-specific policy, but they should not
be the only way to remember a node's prior construction arguments. Replacement needs a
first-class view of the predecessor's typed/structured configuration values so a
handler can express "change these arguments, preserve the rest". The same value model
must handle nested initialization-list/container arguments used by module automation.
The exact replacement/update spelling remains provisional.

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

```cpp
struct ConfigureOscillator {
    float frequency;
    float gain;
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

# `PresentationState`: coherent semantic synchronization

## Presentation state is distinct from runtime node state

A node may expose a presentation-facing semantic schema/projection distinct from
its internal `State` / `TockState`. This document calls that projection
`PresentationState`; the exact declaration syntax is not yet fixed.

`PresentationState` is not a serialization of implementation storage. It may omit
internal fields and include derived semantic fields suitable for presentations.

A node is not required to define custom `PresentationState` merely to have a
presentation. The framework-provided default presentation described later must
work for every node from generic graph/port information and future framework
presentation capabilities.

## All presentations of one backend node share the same presentation state

If several active Vue presentations refer to the same semantic backend node, they
all receive the same authoritative `PresentationState`.

```text
                 semantic backend node
                         |
                  PresentationState
                         |
              +----------+----------+
              |          |          |
              v          v          v
           view A      view B      view C
```

`PresentationState` is not selected independently per presentation. Its purpose is
to make every representation of the same semantic node converge on the same
backend-derived semantic truth.

Presentation-local UI state remains independent. Zoom, hover, expansion, local
selection, drag state, scroll position, and layout choices may differ between
views without becoming backend node state.

## Synchronization occurs after the whole root event interaction

Presentation state must not be emitted piecemeal while a root interaction is still
propagating.

The intended order is:

```text
root UI event
    -> complete ordered backend propagation
    -> determine the final semantic/realization result
    -> serialize PresentationState for affected nodes
    -> publish one coherent presentation update
```

For ordinary node-local mutation, the set of nodes traversed by the event tree is
also the natural presentation invalidation set because nodes cannot mutate one
another directly. The runtime already tracks this set to enforce one-delivery-per-
concrete-node.

Structural replacement may additionally invalidate semantic descendants that were
created, removed, or rebound by that replacement; that bookkeeping must be part of
the resulting presentation commit rather than pretending those changes were
ordinary direct state mutation.

## Ordinary state synchronization requires no explicit `reply` / `notify`

Handlers should not have to remember to notify Vue after mutating semantic state.
After a root interaction succeeds, the runtime automatically produces the relevant
`PresentationState` and publishes it to every active presentation of each affected
semantic node.

Accordingly, an explicit `ctx.reply()` / `ctx.notify()` is not required for this
ordinary synchronization path. Introducing a separate backend-to-presentation
message path would weaken the guarantee by making synchronization depend on
handler discipline.

# Vue presentation API

## Vue holds a semantic `node` object

The presentation-facing abstraction should be intuitive and independent of the
transport implementation:

```ts
const node = useNode<MyNode>()
```

The exact generic/type syntax remains provisional, but the object represents the
semantic backend node being presented. Its stable surface should include the
conceptual equivalents of:

```ts
node.address
node.state
node.send(...)
```

where:

- `node.address` is the generated/typed semantic `NodeAddress`;
- `node.state` is the generated `PresentationState` type for this backend node;
- `node.send(...)` sends a reliable typed semantic event to the node.

Vue should not know concrete node indices, `NodeStorage`, graph generations,
sockets, JSON-RPC method names, or other transport/runtime plumbing.

## Events are strongly typed in TypeScript

Generated frontend event APIs must not degrade to a string event name plus an
untyped/raw JSON object such as:

```ts
// not the intended API
node.send("SetCutoff", { cutoff: 800 })
```

The source-introspection layer should generate concrete strongly typed TypeScript
event values/types from the authoritative C++ event definitions. `node.send(...)`
accepts those typed values (the exact generated construction syntax remains open).
For example, possible generated ergonomics could look like:

```ts
const event: SetCutoff = { cutoff: 800 }
node.send(event)
```

or an equivalently strongly typed generated constructor/API.

JSON is a transport representation, not the programming model. JSON conversion
functions/codecs for each generated event type should be generated automatically
from the same authoritative C++ schema. Vue authors should neither select event
types with raw strings nor manually serialize/deserialize their payload objects.

The same principle applies in the reverse direction to generated presentation
schemas.

## TypeScript generation comes from authoritative C++ definitions

The mandatory source-introspection/compiler layer should generate the frontend
protocol artifacts rather than requiring parallel hand-maintained C++ and
TypeScript schemas.

The generated surface is expected to cover, where relevant:

- concrete strongly typed event/message types;
- generated JSON encode/decode functions or equivalent codecs for those types;
- `NodeAddress` and its codec;
- node `PresentationState` types and codecs;
- static information describing which event types a statically known node type
  supports;
- future live-subscription schema/capability types once that model is settled.

Generated frontend protocol code must not expose backend ABI information such as
state-field offsets, packed `NodeStorage` layout, concrete node indices, migration
metadata, or compiled realization details.

The source-introspection layer may know both sets of facts, but backend ABI metadata
and frontend semantic protocol metadata are different generated artifacts.

## Static event compatibility should be preserved where possible

Because source introspection discovers typed handlers, a statically known
presentation/node type should expose an event-send surface that rejects unsupported
events at TypeScript compile time where practical.

Likewise, statically known C++ `NodeRef` targets can be diagnosed when an event is
known not to be supported. Runtime diagnostics remain necessary for dynamic
selectors whose concrete target set cannot be known statically.

# Live backend-to-presentation information

The exact API in this section is deliberately **not settled**. The constraints are
settled enough to prevent choosing an abstraction that does not fit the application.

## Live information is not `PresentationState`

`PresentationState` has one authoritative value shared by all active presentations
of the semantic node after a semantic interaction.

Live information has different semantics:

- it is observational rather than a semantic mutation result;
- losing intermediate updates is acceptable;
- older updates may be skipped in favor of fresher complete information;
- one presentation may want a different subset than another presentation of the
  same backend node;
- requirements can change while the presentation remains mounted/visible;
- backend work performed only for observation should be avoidable when nobody
  currently demands it.

There is no corresponding unreliable presentation-to-backend state channel.
Backend state remains authoritative, and semantic mutations from Vue are reliable.

## A presentation dynamically manages its live requirements

A visible presentation may change what it needs according to what it is actually
rendering. Zoom level is an important example: zooming in may reveal expensive
visualizations; zooming out should permit those subscriptions and their backend
work to disappear while the presentation remains active.

Thus subscription lifetime is finer-grained than presentation lifetime:

```text
presentation becomes visible
    -> may request some live information

zoom/layout/content changes
    -> add/remove requirements dynamically

presentation leaves visibility
    -> remove its remaining live requirements
```

Different simultaneous presentations of the same node may maintain different
requirement sets. Shared backend work may be reused where the requirements overlap.

## Generalize subscription machinery, not data representations

The live mechanism must not assume that every useful backend value is one of a
closed set of visualization enums such as `Peak`, `RMS`, or `Waveform`.

Port contracts are intentionally heterogeneous. Depending on the port and its
type, meaningful distinctions include:

- sequential versus order-independent/random-access input behavior;
- ephemeral versus persisted output behavior;
- Tick versus Tock production/lifetime;
- one, two, or future higher channel counts;
- planar versus interleaved native sample organization;
- future non-audio port/data contracts.

Those differences affect what data exists, who owns it, how long it remains valid,
and which representation avoids unnecessary conversion. The presentation layer
must not erase them by inventing one fake universal "port visualization" contract.

The common live architecture should instead generalize mechanisms that really are
shared, such as:

- capability/source discovery;
- dynamic subscribe/unsubscribe;
- demand aggregation/reference counting;
- activation/deactivation of observation work;
- scheduling/publication at presentation-scale cadence;
- replaceable/latest-oriented delivery;
- sharing one produced value with multiple interested presentations.

The payload and production semantics may remain heterogeneous and strongly typed.

## Framework and node extensions must use the same live mechanism

The eventual live system should be extensible by both framework/runtime facilities
and node types. It should not contain a special arbitrary path for port information
and an unrelated second path for node-specific runtime information.

For example, the framework/runtime may make some information available from port
contracts without requiring every node author to re-forward its own inputs and
outputs. A node type may additionally expose node-specific information such as a
playback cursor or active-voice count. Both kinds of information should participate
in the same discovery/subscription/lifecycle system even if their internal
producers are completely different.

It is intentionally unresolved whether the right public concept is called a live
property, source, field, feed, capability, or something else.

## Do not force port data through one representation

A future sample-port observation may need to preserve the port's natural channel
count and planar/interleaved representation to avoid pointless copies. Other port
contracts may have no meaningful "recent waveform" at all, or may expose persisted
or random-access data through a mechanism that should not be treated as live
telemetry.

The live architecture must therefore be able to express absence of a capability
and heterogeneous typed data without central enumeration of every representation
the backend could ever produce.

## Live update cadence is presentation-scale, not audio-block-scale

These updates exist for visual presentation. Their publication should be tied to a
reasonable UI/display cadence rather than every audio block. A practical cap may
end up in the approximate 30-60 Hz range or be constrained by the host/browser,
but the exact scheduling policy is intentionally not part of the semantic API yet.

Realtime accumulation and presentation publication also need not have the same
cadence. A provider may perform cheap realtime bookkeeping continuously and expose
a replaceable snapshot only when an active presentation update is due.

# Default presentation

## Every node has a useful default presentation

A node must remain presentable even if its type defines no custom presentation
schema and no custom Vue presentation.

The framework therefore provides a default presentation that adapts to the node's
defined ports and relies on framework-provided presentation capabilities rather
than forcing each node type to explicitly forward its own input/output information.

The current desired default behavior is to consider only the first output, if one
exists, and the first input, if one exists, and render each approximately as:

| Presentation mode | Default first-port visualization |
| --- | --- |
| `nano` | presence/activity indicator |
| `mini` | dB Vue meter |
| `compact` | presence/activity indicator because width is constrained |
| largest/full mode | waveform or short-term waveform |

The older presentation document currently calls the largest/full editing profile
`editable`; the final mode name is not important to this communication contract.

This table specifies the desired presentation behavior, **not** a settled backend
port-observation API. Different port contracts may require different providers or
may not support every visualization in the same way. Determining the faithful live
capability model remains open.

# What remains deliberately unresolved

The following should not be accidentally standardized while implementing the
settled contracts above:

- the exact transaction/journal representation used to prepare and replay one
  ordered root interaction at a pass boundary;
- partial/successor `NodeStorage` migration details;
- exact spelling of `NodeMessageContext`, `ModuleMessageContext`, `on_message`,
  `g.on_message`, selectors, or replacement APIs;
- exact C++ declaration/projection syntax for `PresentationState`;
- exact generated TypeScript construction syntax for event values, beyond the hard
  requirement that it remain strongly typed rather than string/raw-JSON based;
- the exact live-subscription API and terminology;
- the catalog/discovery representation for framework-provided and node-provided
  live capabilities;
- which information each existing port contract can faithfully expose;
- when UI-facing signal processing belongs in backend providers versus Vue;
- exact handling of planar/interleaved and future multi-channel live sample data;
- how persisted/random-access port data should be divided between live observation
  and ordinary data access;
- the exact presentation refresh cadence and host-specific scheduling limits.

The next live-observation design step should therefore begin from concrete
application jobs and information ownership rather than from a universal waveform /
meter enum. For each desired UI datum, identify who owns it, its lifetime/time
semantics, whether missing an update affects correctness, whether producing it
requires observation-specific work, and which representation already exists
natively.
