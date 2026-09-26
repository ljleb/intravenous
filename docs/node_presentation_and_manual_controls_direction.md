# Node Presentation And Manual Controls Direction

_Status: follow-on direction after the core GraphJit/GraphExecutor path is stable.
The scoped GraphBuilder identity work in
[scoped_graph_builder_and_subgraph_closure_direction.md](./scoped_graph_builder_and_subgraph_closure_direction.md)
provides the nested semantic identities used by source focus and presentation
rebinding. Manual-control JIT specialization also depends on the final GraphJit /
GraphExecutor realization-switching contract. This is not an instruction to pull
UI/control work ahead of those runtime and builder milestones._

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

## Presentation interaction state outlives React components

An active gesture must not be owned by the React component that happened to
render the control when the gesture began.

A longer-lived presentation interaction context retains at least:

```text
stable authored node identity
stable input/control identity
pointer/scrub interaction state
dynamic-JIT requirement, when applicable
```

Rendering components bind to that context. Therefore all of the following may
occur without cancelling an active scrub merely because a component remounted:

- resizing a presentation;
- crossing a display-mode threshold;
- selecting another presentation profile;
- hiding/replacing the concrete knob widget;
- rerendering/remounting React components;
- publishing a new project realization that preserves the semantic target.

When a new project revision publishes, both ordinary presentation hosts and
active interaction contexts re-resolve their stable semantic identities against
the new realization.

If the target no longer exists or is no longer compatible, the interaction ends
through an explicit invalidation path rather than through incidental component
lifetime.

## Source navigation uses authored instance + scoped virtual identity

C++ source annotations should resolve presentation/source focus to the semantic
pair:

```text
authored module/project instance
+
scoped virtual-node identity
```

rather than to one generation-local concrete member.

The scoped virtual-node identity comes from the construction-scope model in
[scoped_graph_builder_and_subgraph_closure_direction.md](./scoped_graph_builder_and_subgraph_closure_direction.md).
The sidebar/presentation host can therefore open the authored module instance and
focus its generic subgraph presentation on that virtual node.

If the virtual node currently has several concrete members, the presentation may
show those members as rows/children. If a later realization changes the
member count, the source-level focus may survive because it addresses the virtual
identity rather than one concrete index. If the virtual node disappears, the
enclosing module presentation remains valid and only the focused child target is
lost.

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
output-history/latency semantics remain governed by GraphJit/GraphExecutor state
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

A settled manual value is an exact constant from the graph's point of view. Do
not permanently smooth every control merely because it can be edited.

The intended lifecycle is:

```text
settled:
    exact value / specialization candidate

scrub begins:
    input becomes a dynamic Tick control

for each audio callback while scrubbing:
    latch the newest UI target
    interpolate sample-by-sample from the previous effective value
    to the latched target over the callback

scrub ends:
    stop interpolation
    settle at the final exact manual value
```

The UI-to-audio target scalar still requires defined C++ concurrency. The timing
semantics intentionally allow the audio thread to observe whichever coherent
latest target is available; a relaxed atomic is sufficient for that scalar.
Audio-side previous/effective/interpolation state is owned by the audio thread and
does not require an RPC/queue handshake per target update.

## Predictive dynamic-JIT specialization

Settled controls are candidates for constant specialization under GraphJit. A
control that is about to become interactive needs a code variant in which the
relevant manual input is dynamic.

The initial policy is deliberately simple:

1. settled/manual inactive controls may be specialized as constants;
2. hovering/engaging a participating control requests a variant whose
   `dynamic_inputs` set contains that input, so compilation can begin before the
   first meaningful scrub movement;
3. another relevant hover/gesture updates the requested set and supersedes the
   older request;
4. simultaneous gestures are represented as a set, not one optional active
   knob:

   ```text
   dynamic_inputs = { A, B, ... }
   ```

5. initially there is no recent-variant cache; compile/reuse policy can be
   revisited only after profiling;
6. a compilation that cannot be cancelled inside LLVM carries exact
   project-revision/request identity, and its result is discarded if stale;
7. on release, a settled constant-specialized variant is requested again;
8. until that settled variant is ready, the currently active dynamic variant
   continues reading the final unchanged manual scalar.

This is code specialization, not graph/state replacement.

## All compatible control variants share NodeLayout and state semantics

Dynamic and constant-specialized variants for one structural graph realization
must use the same canonical `NodeLayout` and `NodeStorage`. Switching between
those variants is therefore code-only and requires no storage migration.

The compatibility rule is stronger than identical offsets:

> Code variants declared layout-compatible must preserve compatible persistent-
> state semantics, not merely the same byte layout.

A constant specialization cannot stop updating persistent state that a later
switch-compatible dynamic variant expects to continue from. Any specialization
that changes graph structure, state identity, initialization/lifecycle, or
`NodeLayout` is a structural recompilation rather than a code-only control
variant.

## Relationship to GraphJit value specialization

Manual controls are one concrete application of the broader value-specialization
mechanism described in [graph_jit_direction.md](./graph_jit_direction.md).

The specialization key needs both:

```text
structural graph revision identity
exact dynamic-input set / settled specialization values
```

Stale-result rejection therefore follows the same exact-generation principle as
other GraphJit work. Control hover/scrub state may request compilation, but it
never directly activates stale native code; GraphExecutor still owns safe
activation of an already-compiled compatible realization/variant.

## Relationship to project persistence

Manual-control values and their participation policy are semantic project state.
They must eventually serialize with the owning project node/input identity.

Do not persist:

- the currently active code specialization;
- hover state;
- pointer/scrub state;
- interpolation progress;
- pending LLVM requests;
- generated variant machine code.

Those are realization/interaction details reconstructed from the durable graph and
current UI interaction.

## Implementation order

This work follows the runtime and builder prerequisites rather than leading them:

1. finish GraphJit and GraphExecutor;
2. perform the planned large optimization/profiling iteration;
3. perform the scoped GraphBuilder/session-identity migration where required for
   stable nested virtual identities;
4. establish presentation rebinding around authored/scoped semantic identities;
5. add persistent manual values/participation policy to project state;
6. add long-lived gesture contexts and scrub-only interpolation;
7. add predictive dynamic-input specialization on top of the settled GraphJit
   value-specialization/GraphExecutor activation machinery;
8. profile before adding recent-variant caches or more elaborate hover policies.

The key architectural boundary is that presentations and manual controls consume
stable graph/runtime identities; they do not become an alternate owner of graph
construction or runtime state.
