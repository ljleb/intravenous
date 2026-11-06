# Node Presentation And Manual Controls Direction

> **Communication model:** backend-node typed events, shared `PresentationState`,
> Vue `useNode()` semantics, the language-neutral backend protocol plus
> frontend-owned language bindings, and live
> presentation-subscription constraints are now consolidated in
> [Node Interaction And Presentation Communication Direction](./node_interaction_and_presentation_communication_direction.md).
> This document remains the home for display modes, interaction lifetime,
> persistent manual controls, and control/JIT-specialization direction.

_Status: normative Front 14 direction. Front 13 supplies stable `NodeAddress` and
selector semantics. Front 6 supplies revision continuity. Front 12 can improve later
scoped construction, but it does not block the initial Front 14 implementation._

The superseded JIT scheduling and dependency text is preserved in
[historical documentation](./historical/front_14_manual_control_jit_policy_before_demand_activation_2026-11-04.md).

This document consolidates the planned node-presentation, interaction-lifetime,
manual-control, and control-specialization model.

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

## Four global display modes

A presentation receives exactly one of four global display modes:

| Mode | Contract |
| --- | --- |
| `compact` | Width is below the globally guaranteed normal-presentation width. |
| `nano` | Read-only temporal/activity-oriented presentation. |
| `mini` | Read-only structured/spatial presentation. |
| `editable` | Full editing model from the editing threshold through fullscreen. |

`compact` supersedes the normal height/profile modes at narrow width. It does not
combine independently with `nano`, `mini`, and `editable`; there are four modes,
not a width x height Cartesian product.

There is no required left-anchor rule. Presentation layout is responsible for
responding to the supplied mode and available host geometry.

## Presentation interaction state outlives Vue components

An active gesture must not be owned by the Vue component that happened to
render the control when the gesture began.

A longer-lived presentation interaction context retains at least:

```text
stable authored node identity
stable input/control identity
pointer/scrub interaction state
latest desired manual value
required `dynamic_inputs` membership, when applicable
```

Rendering components bind to that context. Therefore all of the following may
occur without cancelling an active scrub merely because a component remounted:

- resizing a presentation;
- crossing a display-mode threshold;
- selecting another presentation profile;
- hiding/replacing the concrete knob widget;
- rerendering/remounting Vue components;
- publishing a new project realization that preserves the semantic target.

When a new project revision publishes, both ordinary presentation hosts and
active interaction contexts re-resolve their stable semantic identities against
the new realization.

If the target no longer exists or is no longer compatible, the interaction ends
through an explicit invalidation path rather than through incidental component
lifetime.

For socket RPC clients, the server assigns the interaction's client identity from
the authenticated connection. A request cannot claim another client's identity.
Disconnect removes that client's hover and active-interaction state. The durable
manual value remains the last value accepted before disconnect.

## Source navigation uses `NodeAddress` and its selector path

C++ source annotations resolve presentation and source focus to the semantic pair:

```text
NodeAddress::instance_id
+
NodeAddress::path
```

Together, these fields form the stable `NodeAddress` supplied by Front 13. Presentation
code does not retain one generation-local `NodeBundleHandle` or concrete member index.

`NodeAddress::path` contains `ProjectNodePathSelector` values. Each value is a
`ProjectVirtualNodeSelector`, `ProjectVirtualMemberSelector`, or
`ProjectTiledChildSelector`. The sidebar can open the authored module instance and
focus its generic subgraph presentation on that semantic target.

If the virtual node currently has several concrete members, the presentation may
show those members as rows/children. If a later realization changes the
member count, the source-level focus may survive because it addresses the virtual
identity rather than one concrete index. If the virtual node disappears, the
enclosing module presentation remains valid and only the focused child target is
lost.

Front 12 can add same-session scoped construction and `SubgraphClosure`. It must
preserve these semantic selector paths. Front 14 does not wait for Front 12.

## Reconstruction and state continuity

Changing construction/configuration arguments produces a new graph realization
while retaining the authored project node instance identity:

```text
same authored node identity
new construction inputs
new concrete realization
```

That stable identity means "candidate continuation", not "reinterpret arbitrary
old state as the new type".

Leaf state migration remains subject to compatible generated/native state
identity and migration rules. Module state reconciliation recursively uses its
stable virtual/concrete child identities. Node-owned input-history and
output-history/latency semantics remain governed by GraphJit/executor state
reconciliation, not by presentation ownership.

Presentations simply rebind to the newly published compatible realization.

## Persistent manual controls

An eligible sequential input may have persistent project-owned manual-control
state.

The state contains at least a stored manual value plus the input's manual
participation policy. The intended behavior is:

```text
eligible sequential input
    has persistent manual-control value

no external source + manual mode enabled
    manual value participates

external source present + disconnected-only mode
    manual value remains visible/editable/persisted
    manual source does not participate

external source present + participate-with-connections mode
    manual source participates according to the input's defined
    composition/fan-in semantics
```

Connecting an external source does not erase the manual value. Disconnecting it
does not recreate a default from scratch. The stored value survives ordinary
connection edits and belongs in durable project state/JSONL once that persistence
surface is implemented.

The live scalar may reside in `NodeStorage`; one scalar per eligible/manual control
is not a reason to create a separate runtime storage architecture.

## Manual values are Tick/ephemeral sources

A participating manual control is semantically a live Tick/ephemeral source,
analogous to another live real-time input.

Therefore it can satisfy ordinary Sequential consumption, but it does not by
itself satisfy a Random Access requirement. Random Access behavior follows the
normal graph capability/retention rules; do not add a manual-control-specific
exception that makes an ephemeral live scalar retrospectively addressable over
arbitrary positions.

This keeps manual controls within the existing port/access model instead of
creating a second control-only execution semantics.

## Interpolation exists only during active scrubbing

A settled manual value is eligible for exact constant specialization. The stored
manual value is the latest value retained by the project and UI. The effective manual
value is the value used by the active executable graph. These values can differ while
a replacement graph compiles.

Hover requests compilation of dynamic-input code. Hover does not activate that code.
If scrubbing starts first, the UI updates the stored value while the active graph keeps
using its existing constant. No generic execution path applies the new value to code
that does not support a dynamic input.

The intended lifecycle is:

```text
settled:
    active graph uses the exact constant value

hover:
    request dynamic-input compilation
    keep the active graph unchanged

scrub begins before dynamic code is active:
    retain each latest desired value
    keep the active graph's constant effective value

dynamic graph becomes active during the scrub:
    make the latest desired value effective
    begin audio-thread interpolation from the prior effective value

for each audio callback while dynamic scrubbing is active:
    latch the newest UI target
    interpolate sample-by-sample from the previous effective value
    to the latched target over the callback

input updates become idle:
    stop interpolation
    keep the dynamic graph active during the debounce interval

debounce interval expires:
    capture the latest stored value
    request a constant-specialized replacement

constant-specialized graph becomes active:
    use the settled value as an exact constant
```

Pointer release is not the only idle trigger. Front 14 uses one explicit configurable
or fixed debounce policy instead of compiling after every input update.

The UI-to-audio target scalar requires defined C++ concurrency. The audio thread can
observe the latest coherent target that is available. A relaxed atomic is sufficient
for that scalar. The audio thread owns the prior value, effective value, and
interpolation state. A target update does not require an RPC or queue handshake.

## Predictive compilation and demand-driven activation

Settled controls are candidates for constant specialization under GraphJit. Hovering
over a participating control requests code whose `dynamic_inputs` set contains that
input. Hover alone does not activate the compiled code.

Front 14 initially keeps one active executable graph and one logically current
replacement request. Compiled code can overlap temporarily during safe activation and
reclamation. Front 14 does not keep a cache of old control specializations.

The required `dynamic_inputs` set contains every control with an ongoing interaction.
It can also contain the currently relevant hovered control for predictive compilation.
A later hover can replace the predictive part of the set. It must not remove a control
that an ongoing interaction still requires.

The initial scheduling policy is:

1. A settled control can be an exact constant in the active graph.
2. Hover requests a dynamic replacement but keeps the active graph unchanged.
3. An interaction makes its control a required member of `dynamic_inputs`.
4. If matching dynamic code is ready, `GraphExecutor` activates it safely.
5. If matching code is not ready, the UI retains the latest desired value.
6. The active graph continues to use the value representation that it supports.
7. If the interaction ends first, the scheduler can skip the obsolete dynamic result
   and request the settled constant replacement directly.
8. After the idle interval, the scheduler requests code with the latest settled
   constant values.

Each replacement request carries the exact project revision, request identity,
`dynamic_inputs` set, and settled values. A newer relevant request supersedes the
older request. The compilation worker cancels queued or active work when its LLVM
operation supports cancellation. Otherwise, the worker finishes the obsolete work
and discards its result.

Compilation below approximately 200 ms is a performance target from an early proof of
concept. It is not a guaranteed latency. Front 16 can add reuse or more scheduling
machinery only after measurements show a benefit.

This process changes code specialization. It does not replace graph or node state.

## Compatible control code shares NodeLayout and state semantics

Dynamic and constant-specialized code for one structural graph realization must use
the same canonical `NodeLayout` and `NodeStorage`. A compatible activation is code-only
and requires no storage migration.

The compatibility rule is stronger than identical offsets:

> Compiled code declared layout-compatible must preserve compatible persistent-
> state semantics, not merely the same byte layout.

A constant specialization cannot stop updating persistent state that compatible
dynamic code expects to continue from. A specialization is structural if it changes
graph structure, state identity, initialization, lifecycle, or `NodeLayout`.

## Relationship to GraphJit value specialization

Manual controls are one concrete application of the broader value-specialization
mechanism described in [graph_jit_direction.md](./graph_jit_direction.md).

The specialization key needs both:

```text
structural graph revision identity
exact dynamic-input set / settled specialization values
```

Stale-result rejection follows the same exact-generation principle as other GraphJit
work. Control hover and scrub state can request compilation. They never directly
activate native code. `GraphExecutor` owns safe activation of compatible compiled
code.

## Relationship to project persistence

Manual-control values and their participation policy are semantic project state.
They must eventually serialize with the owning project node/input identity.

Do not persist:

- the currently active code specialization;
- hover state;
- pointer/scrub state;
- interpolation progress;
- pending LLVM requests;
- generated machine code.

Those are realization/interaction details reconstructed from the durable graph and
current UI interaction.

## Live observation is outside Front 14

Front 14 does not define the live-observation API. The capability, subscription,
representation, and scheduling questions remain unresolved in
[Node Interaction And Presentation Communication Direction](./node_interaction_and_presentation_communication_direction.md).
The default presentation can use a later live-observation system, but Front 14 does
not need that system to implement presentation identity and manual controls.

## Implementation order

Front 14 uses the completed identity, persistence, GraphJit, and executor contracts:

1. Define `NodePresentation`, the framework default presentation, and the four display
   modes.
2. Rebind presentation hosts and long-lived interaction contexts through stable
   semantic identities.
3. Store manual values and participation policies in durable project state.
4. Realize participating manual values as Tick/ephemeral inputs.
5. Add audio-thread-owned interpolation for active dynamic scrubbing.
6. Add one active executable graph and one current replacement request.
7. Add predictive hover compilation, demand-driven activation, stale-result rejection,
   and the idle return to constant specialization.
8. Measure compile latency and interaction latency before Front 16 adds reuse or more
   scheduling machinery.

### Implementation status (recorded 2026-11-04, verified)

The manual-control backend is implemented and verified: the repository owner's complete
test suite has passed with it included (status corrected 2026-11-06). `ProjectManualControl` is durable project state keyed by
`ManualControlId`, which contains a stable `NodeAddress` and authored input name.
`ProjectGraph` resolves that identity for each `ConfiguredGraph`, owns client-scoped
hover and interaction lifetimes, and runs the one-request predictive/required
specialization worker. Project persistence and socket RPC preserve the durable value
and participation policy; the socket server supplies the client identity.

GraphJit currently accepts scalar `Sample` Sequential Tick inputs. This is an initial
implementation limit, not a rule that all future manual controls must use `Sample`.
It uses the node's existing Sequential combination callback for a connected input,
maintains one canonical input timeline across constant and dynamic code, and performs
scrub interpolation across the complete audio callback even when the node runs in
smaller primitive slices. Constant code uses compiler-owned immutable sample storage.
Dynamic code reads the latest atomic target and owns the effective interpolation
value in `ManualControlRuntimeState`.

`GraphExecutor::stage_realtime_code()` accepts only code whose project generation,
`NodeLayout`, state structure, lifecycle callbacks, array bindings, and manual-control
bindings are compatible with the accepted structural generation. It changes the
steady Tick operations at `activate_pending()` without rebuilding `NodeStorage`.

The presentation registration and selection path is implemented and verified (status
corrected 2026-11-06). `IV_NODE_PRESENTATION` emits a neutral
package record. The package finalizer, loader, accepted revision, package catalog, and
socket RPC preserve the record. The VS Code frontend owns
`NodePresentationRegistry`, its registered providers, and its guaranteed default
provider. The current implementation does not add live observation. It also does not
define renderer data for a future Vue or C++ frontend.

The key architectural boundary is that presentations and manual controls consume
stable graph/runtime identities; they do not become an alternate owner of graph
construction or runtime state.
