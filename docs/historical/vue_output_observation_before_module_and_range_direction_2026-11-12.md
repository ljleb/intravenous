# Vue Observation Direction Before Output Queries And `Observations` Ownership (2026-11-12)

_Historical, non-normative. These excerpts were superseded on 2026-11-12 by_
[Vue Output Observation And Query Direction](../vue_output_observation_direction.md)
_and corresponding clarifications in their source documents. Each excerpt below
_is the original text retained verbatim, including previously unresolved choices._

## Source: `docs/node_interaction_and_presentation_communication_direction.md`

```markdown
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

Use existing `NodeStateStructure`/`NodeStateFieldStructure` reflection for the
single authored `State` (with its execution-domain classification) and resolved port
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
values, and some values require a producer or post-operation boundary. Tick-owned
and Tock-owned `State` have different thread ownership. Non-realtime code must not
read mutable realtime storage concurrently without a safe snapshot/handoff.

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
```

## Source: `docs/node_interaction_and_presentation_communication_direction.md`

```markdown
`State` fields (Tick or Tock) and port data. Exact observation hook syntax is deferred;
```

## Source: `docs/node_interaction_and_presentation_communication_direction.md`

```markdown
The generic presentation can inspect ports and their
available operations without any node-specific TypeScript source.
```

## Source: `docs/node_interaction_and_presentation_communication_direction.md`

```markdown
Ports stay connectable even
when their live visualizations are hidden at a given scale.
```

## Source: `docs/node_interaction_and_presentation_communication_direction.md`

```markdown
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
```

## Source: `docs/node_presentation_and_manual_controls_direction.md`

```markdown
The observation mechanism and its limits are specified in
[Node Interaction And Presentation Communication Direction](./node_interaction_and_presentation_communication_direction.md).
```

## Source: `docs/project_graph_application_architecture.md`

```markdown
| `SocketRpcServer` | transport JSON-RPC requests/notifications without becoming the owner of project state |
```

## Source: `docs/project_graph_application_architecture.md`

```markdown
Future presentation modules may include `PresentationDefinitions` and
`PresentationInstances`. They are intentionally not required for the initial
project-graph implementation.
```

## Source: `docs/project_graph_application_architecture.md`

```markdown
## Presentations

The initial project-graph work does not require a generic automatically managed
subgraph/controller layer. Presentation selection belongs to the Vue frontend:
there is no backend `NodePresentation` registration extension or package-owned
frontend-provider compatibility contract in the intended architecture. A generic
Vue presentation is always available, while specialized presentations select nodes
by stable leaf or module definition ID.

A future application module such as `PresentationInstances` may own reconciled
client observation interests, shared producer activation, collected observation
results, and batched delivery. It need not be a presentation-component registry
and does not necessarily require its own thread. This ownership remains subject to
concrete Vue integration. Subscription changes and observations use the same
application-module batching discipline as other project operations, but replaceable
observation frames remain separate from durable graph/semantic notifications.

A presentation requiring structural graph changes routes them through the same
`ProjectGraph` root-build transaction, not directly through independently converging
`NodeInstances` and `GraphConnections` paths. A purely visual presentation need not
own graph state at all.

A module publishing asynchronous UI updates to `SocketRpcServer` does so as a
separate source/cause from an incoming JSON-RPC request. `SocketRpcServer` may be a
request-tree root and a notification-tree sink, but the same cause must not leave and
then re-enter it.

The intended four global display modes, stable interaction rebinding, and manual
control/JIT-specialization semantics remain in
[node_presentation_and_manual_controls_direction.md](./node_presentation_and_manual_controls_direction.md).
Reflected state/port observations, type-registered operations, and minimal
observation-aware GraphJit replanning are described in
[node_interaction_and_presentation_communication_direction.md](./node_interaction_and_presentation_communication_direction.md).
The former backend-presentation-module proposal is preserved in
[historical documentation](./historical/node_presentation_and_live_observation_before_vue_direction_2026-11-12.md).
```

## Source: `docs/random_access_port_data_direction.md`

```markdown
### Type-registered live observation operations (planned)

The same closed registered continuous-value type system should also advertise a
finite set of **optional** observation operations suitable for each type. The
operation's supported source shape, resolved dimensions/channels, capture contract,
and output schema are type-specific. An implementation shared by multiple types may
be expressed once as a generic template, but each type explicitly opts in; there is
no universal `Peak`/`Waveform` switch and no implicit transformation between
continuous value types.

For example, `Sample` may support latest-value, sample-level, and bounded waveform
observations as distinct operations; `FFTBlock` may support a spectrum projection,
while `FractionalGlobalIndex` may only support latest-value observation. The set
actually implemented should be driven by concrete Vue usage. Availability may also
depend on whether the resolved source is ephemeral Sequential, persisted, or
random-access. Observation operations do not change the existing authored-port,
connection, storage, or value-conversion semantics just by being registered.

The operation interface, selective capture placement, and collection/batching
lifecycle are specified in
[Node Interaction And Presentation Communication Direction](./node_interaction_and_presentation_communication_direction.md).
```

## Source: `docs/node_presentation_and_manual_controls_direction.md`

```markdown
Front 14 did not implement the live-observation API. The intended direction is now
recorded in [Node Interaction And Presentation Communication Direction](./node_interaction_and_presentation_communication_direction.md):
frontend-selected registered observation operations over reflected state fields and
ports, selectively planned GraphJit captures, and batched subscriptions and updates.
The concrete frontend and transport interfaces are intentionally deferred until a
real Vue presentation requires them. Manual controls do not depend on implementing
live observation first.
```

## Source: `docs/implementation_fronts_direction.md`

```markdown
Front 14 did not implement live observation. The newly accepted observation direction
uses reflected fields and ports, type-registered operations, minimal GraphJit capture
planning, and batched/reactive Vue interests and updates. Its concrete interfaces
remain future implementation work driven by a real Vue client, not new speculative
Front 14 routes or node callbacks.
```

## Source: `docs/project_session_server_direction.md`

```markdown
- Snapshots and notifications carry a monotonically increasing session
  `revision`.
```

## Source: `docs/node_interaction_and_presentation_communication_direction.md`

```markdown
synchronization, and Vue-driven live observation. The observation operation
contract is intentional; its exact C++/TypeScript syntax and transport routes remain
provisional until exercised by the Vue client.
```

## Source: `docs/README.md`

```markdown
- [Node Interaction And Presentation Communication Direction](./node_interaction_and_presentation_communication_direction.md)
```
