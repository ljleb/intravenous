# Presentation And Live Observation Direction Before The 2026-11-12 Vue Revision

_Status: historical, non-normative. Preserved on 2026-11-12 from the uploaded
repository documents to record the superseded design and its rationale. The
replacement intentions are in
[Node Presentation And Manual Controls Direction](../node_presentation_and_manual_controls_direction.md)
and [Node Interaction And Presentation Communication Direction](../node_interaction_and_presentation_communication_direction.md).
No implementation removal is implied by this archival copy._

This revision replaces mandatory backend `NodePresentation` registration with
frontend-owned Vue presentation selection, defers the unimplemented
`PresentationState` projection, and refines live observation around reflected fields,
registered type operations, GraphJit capture planning, and batched interests and
updates. The text below is copied from the prior normative documents; it is
preserved as design history, not as current intent.

## Prior presentation registration and provider selection

_Source: `docs/node_presentation_and_manual_controls_direction.md`, from
`## One presentation extension type` up to `## Four global display modes`._

## One presentation extension type

Every node kind may register zero or more instances of one presentation extension
type:

```text
NodePresentation
```

Do not create parallel extension families for root nodes, module nodes, compact
nodes, or editable nodes. A host selects which registered presentation
is active for the current context.

The project root is not a special presentation concept. It is one node being
presented and may use the same host/presentation mechanism as any other node.

A presentation may be generic (for example, a standard subgraph/member
presentation) or provider-specific. Selection policy belongs to the presentation
host, not to the execution graph.

A generic presentation can generate controls from Front 9 configuration metadata. It
can also provide direct source editing when a generated control is not sufficient. A
provider-specific Vue presentation can update construction arguments through the same
typed value model. The user does not have to edit the expression source for each
change. See
[Configuration Expression Language Direction](./configuration_expression_language_direction.md).

## Package registration and frontend providers

`IV_NODE_PRESENTATION` adds one `PackageNodePresentation` record to an IV package.
The record has three text fields:

- `definition_id` names an `IV_NODE` or `IV_MODULE` in the same package.
- `presentation_id` is the compatibility ID for a frontend provider.
- `display_name` is diagnostic text.

For example:

```cpp
IV_NODE_PRESENTATION(
    "iv.example.filter",
    "iv.example.filter.controls",
    "Filter controls");
```

The package finalizer rejects an empty ID, an unknown `definition_id`, and a duplicate
pair of `definition_id` and `presentation_id`. Different definitions can use the same
`presentation_id`. This permits one frontend provider to present several compatible
node definitions.

The finalized package exposes the records through
`iv_package_node_presentations`. `ModuleLoader` copies each ABI record into an owned
`NodePresentationDescriptor`. `PackageRevision` retains these descriptors, and
`IvPackageInfo::node_presentations` exposes them in the package catalog. The JSON field
is `nodePresentations`.

`IntrospectionVirtualNode::Member::definition_id` exposes the existing
`RegisteredNodeTypeIdentity::node_type_id` for a registered leaf node. The enclosing
`IntrospectionVirtualNode::definition_id` has a value only when all concrete members
have the same non-empty definition ID. `VirtualNodeInfo` and live-graph JSON preserve
both fields as `definitionId`. For an `IV_MODULE`, the host uses the existing
`IvModuleInstanceInfo::definition_id`, which socket RPC already exposes as
`definitionId`. A host therefore matches a descriptor to the same stable definition
ID that created the leaf node or module instance. It does not match a descriptor to
`type_identity`.

The backend does not store a Vue component name, a TypeScript import, or another
frontend implementation detail. A frontend owns its providers. The current VS Code
frontend stores them in `NodePresentationRegistry` by `presentationId`.

`NodePresentationRegistry::select()` accepts a node definition ID, the backend
descriptors, and an optional preferred presentation ID. It uses the preferred provider
when that provider is available. Otherwise, it uses the first available provider in
stable presentation-ID order. If no provider matches, it uses
`frameworkDefaultNodePresentationProvider`. Thus, an unavailable package presentation
cannot make a node impossible to present.

The result is `NodePresentationSelection`. Its `descriptor` is the matching backend
descriptor or `null` for the default. Its `provider` is the selected frontend object.
Its `usedDefault` field tells the host whether it used the fallback.

`NodePresentationProvider` contains only `presentationId`. A frontend-specific provider
type can add its renderer or component fields. `NodePresentationDisplayMode` defines
the four host modes. Each frontend provider must handle all four modes. The package ABI
does not select a mode.


## Prior `PresentationState` contract

_Source: `docs/node_interaction_and_presentation_communication_direction.md`,
from `# PresentationState` up to `# Vue presentation API`._

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


## Prior Vue semantic node surface

_Source: `docs/node_interaction_and_presentation_communication_direction.md`,
from `## Vue holds a semantic node object` up to `## Events are strongly typed`._

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


## Prior live-observation constraints, default policy, and open questions

_Source: `docs/node_interaction_and_presentation_communication_direction.md`,
from `# Live backend-to-presentation information` to end of document._

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
- exact C++ declaration/projection syntax for `PresentationState`;
- exact language-specific construction syntax for event values. This belongs to each
  frontend adapter or supplementary binding-generator plugin, not the backend;
- how a Vue adapter watches `projectGraphGeneration` / `definitionsGeneration`,
  regenerates or reloads bindings during development, and integrates them into its
  build tool. These are frontend integration choices over the settled neutral
  package/runtime protocol;
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

## Prior related architectural passages

_Source: `docs/project_graph_application_architecture.md`, section
`## Presentations` (through `## Immediate implementation order`)._

## Presentations

The initial project-graph work does not require a generic automatically-managed
subgraph/controller layer.

A future presentation system may use:

- `PresentationDefinitions` for available presentation kinds/providers; and
- `PresentationInstances` for live presentation state and presentation-owned
  graph requests.

A presentation that needs structural graph changes should route those changes
through the same `ProjectGraph` root-build transaction rather than reaching
`NodeInstances` and `GraphConnections` through a second converging path. A purely
visual presentation need not own graph state at all.

`PresentationInstances` may later publish asynchronous UI updates to
`SocketRpcServer` so webviews do not poll. Such a notification is a separate
source/cause from an incoming JSON-RPC request: `SocketRpcServer` may be a root
in the request tree and a child/sink in the notification tree, but the same
cause must never leave and then re-enter it.

Detailed presentation event flows are deferred until the core execution graph
is implemented. The intended follow-on presentation/manual-control semantics are
now consolidated in
[node_presentation_and_manual_controls_direction.md](./node_presentation_and_manual_controls_direction.md):
one `NodePresentation` extension family, four global display modes, stable
interaction rebinding, persistent manual-input values, scrub-only interpolation,
and code-only dynamic-input specialization over one compatible `NodeLayout`.


_Source: `docs/package_pipeline_architecture.md`, the original
`NodePresentationDescriptor` paragraph in `## PackageDefinitions`._

An accepted `PackageRevision` also retains its `NodePresentationDescriptor` values.
After `NodeDefinitions` publishes the global definition namespace,
`IvPackageInfo::node_presentations` exposes only descriptors whose `definition_id` is
present in that package's published module or leaf IDs. A definition namespace
conflict cannot publish a presentation descriptor for the rejected definition.
