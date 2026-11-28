# Vue Output Observation And Query Direction

_Status: normative intended design, not a claim of implementation. Observation
routes, C++ operation signatures, and Vue composables are intentionally not fixed
until a complete Vue feature needs them._

This document is the primary contract for observing graph outputs and selected
realtime state from the Vue client. It complements
[Node Interaction And Presentation Communication Direction](./node_interaction_and_presentation_communication_direction.md)
(semantic events and frontend bindings),
[Project Graph Application Architecture](./project_graph_application_architecture.md)
(application-module ownership),
[Coverage And Background Evaluation](./coverage_and_background_evaluation.md)
(random-access and persisted data), and
[Random-Access Port Data Direction](./random_access_port_data_direction.md)
(type ownership).

## 1. Scope and invariants

- Observation is **read-only** with respect to graph semantics. It may cause
  otherwise unnecessary computation, select output materialization, or prepare
  temporary observation storage; it must not alter output values, authored
  retention, connection semantics, or the interpretation of `Node::State`.
- An input observation reads the effective value at the receiving port after
  fan-in, conversion, timing, and manual or neutral fallback. A public module
  input is observed before module-internal routing or conversion.
  [Previous observation scope](historical/input_observation_and_disabled_knobs_before_2026-11-18.md#observation-scope-and-background-queries).
- Selected **Tick `Node::State` fields** may be observed if supported by the global
  field-type codec/capture registry. **Tock `Node::State` field observation is
  explicitly outside the initial scope**, notwithstanding common authored
  `Node::State` naming and reflection. Its diagnostic usefulness and scheduling
  semantics will be reconsidered only after a concrete requirement. Tock *ports*
  are in scope. Internal Tock cache state is never the authority for output data.
- The frontend owns Vue presentation selection, not C++ definitions. There is no
  intended `IV_NODE_PRESENTATION`, `PresentationInstances`, or C++
  `PresentationState` API. A generic Vue presentation works with arbitrary
  reflected ports; specialized presentations may select supported fields and
  operations statically using generated typing. Presentations request each
  `Node::State` field they display explicitly.
- No ordinary node needs `publish()`, `on_subscribe`,
  `on_subscription_change`, or a reused `on_message` for observation. New
  node-authored callbacks and field-specific codec overrides are deferred until
  independently justified. The executor places requested observation work.

## 2. One operation catalog, type-owned capabilities

An observation request identifies a semantic source, a **registered observation
operation**, and parameters. Operation identity describes the result's meaning,
not an implementation-specific cache or transport method.

The continuous-value and event-type registries advertise finite, explicit sets of
supported operations. They need not become a single C++ registry or share storage
implementations, but expose the same *logical* capability model to clients.
Common implementation templates are encouraged; **each type opts into each
operation** and supplies the code or metadata particular to its shape, channels,
value domain, and semantics. GraphJit and the application module must not contain
one growing switch for every presentation type.

Registered capabilities describe the meaningful parameter domain, result schema,
source forms for which an operation is valid, and result/accuracy semantics. A
resolved port's production, retention, exact coverage, and dimensions further
constrain the offered operations. One operation name may have different capture
backends but must not promise inconsistent results for the same parameter set.
A Vue presentation dynamically discovers available operations; its type-specific
bindings may validate known operations and results statically.

The initial operation families should be driven by real Vue usage:

| Source or type | Candidate operation | Meaning |
| --- | --- | --- |
| Supported Tick state scalar | `latest` | Replaceable coherent field snapshot |
| `Sample` output | `latest` | Recent scalar/block where meaningful |
| `Sample` output | `level` | Sample-level interval reduction for a meter; not true peak |
| `Sample` output | `waveform` | Bounded live history or requested time window |
| Continuous spectrum-like type | Type-provided spectral projection | Value-specific spectral representation |
| Event output | `events`, `activity`, `density` | Exact bounded occurrences, activity summary, or per-window counts |

These are illustrative operation names, **not a mandatory universal enum** or
an implementation checklist. A waveform request must say whether it requests
representative sparse samples or a range envelope when that distinction affects
accuracy. Event-density reductions must not silently masquerade as complete
lossless event histories.

### Multichannel sample observation and meter display (owner refinement, 2026-11-16)

A `Sample` output's `level` result preserves the resolved physical channel
layout. A stereo producer yields distinct left/right channel measurements,
not a single mixed-down lane meter. The operation's result shape and any
channel selection must let Vue render the same independent channels that the
source actually produces. A missing or unavailable observation is not a
zero-level channel. Preserve the meaning of sample-peak/RMS reductions;
`level` is not a true-peak operation.

The [Vue meter direction](./vue_presentation_refinement_direction.md#1-channel-correct-meters-and-two-display-modes)
requires green dBFS-graded **level** and linear-magnitude **value** modes.
Both render the same per-channel peak/RMS measurements; only their visual
scale changes. The peak marker's hold, accelerating fall and momentum reset
are client presentation state, not extra audio-thread capture work. The
previous latest-signed-sample proposal is retained
[historically](./historical/vue_meter_value_before_shared_peak_display_2026-11-16.md).
An explicitly requested raw sample-value operation may still exist independently;
it is not the source for these two meter scales. Reuse ruler-grade visual
presentation with square corners rather than inventing another tick system. The meter presentation occupies the **full
remaining horizontal width** of its lane after necessary controls and labels;
independent stereo channels share that full-width display without collapsing
into a single measurement. These are new requirements, not a claim that the
existing Stage 4 client renders them.

For reflected Tick `State`, start with a fixed, explicit set of supported
field types in a **global type codec/capture registry**. Discoverability from
`NodeStateStructure`/`NodeStateFieldStructure` does not imply observability:
unsupported pointers, dynamic containers, and other incompatible fields simply
have no observation capability. The type registry supplies safe capture and
wire representation; optional field-specific overrides are later work. Port
observation operations may reuse those codec primitives without forcing all
`Node::State` types into the continuous-port value registry.

An operation needs the equivalent of describe/prepare/capture/collect behavior,
but those are **responsibilities**, not necessarily four public callbacks.
Capturing an ephemeral runtime value and projecting an immutable persisted range
are different implementations of a type-registered operation; a persisted query
must not be turned into a fake realtime probe merely to use the same interface.

### Deferred `Sample` amplitude-density and time-frequency views (owner intent, 2026-11-16)

These are **future output-observation capabilities**, not Stage 4 meter
completion criteria, new node port kinds, or an instruction to implement them
now. They belong to the type-registered `Sample` operation/presentation model.
Do not fold them into the existing `level`/`value` meter scale switch: those
modes show peak/RMS magnitudes, whereas the following views examine different
properties of the source signal.

**Time-local amplitude density.** Offer a live distribution of *signed* `Sample`
values over an amplitude axis running from `-1` to `+1` by default, with
density/intensity on the other axis. Every observed sample contributes to a
recent-history distribution; older contributions fade with a configurable
exponential decay time. Closely spaced sample values should reinforce one
another smoothly. A narrow Gaussian kernel density estimate is an appropriate
semantic model; an efficiently implemented, fractionally deposited histogram
with optional kernel smoothing is equally acceptable. This is primarily a way
to see where a signal spends its recent time, including stable value clusters
and changing distributions; it is **not primarily a clipping detector**, level
meter, waveform, or frequency analyzer. It intentionally discards sample order.

A fine, **exclusive amplitude bin per rendered horizontal pixel** is a sensible
initial display-resolution target and is not inherently excessive at ordinary
viewport widths. The eventual UI may expose a quality/bin-resolution setting;
resolution and Gaussian bandwidth are distinct controls and must not be
conflated. The observation result and aggregation work must remain bounded as
the view resizes or several clients subscribe. Do not issue one full Gaussian
rasterization over every display pixel *per sample on the audio thread*; capture
and reduce through the established bounded realtime handoff and background or
client-owned presentation work as appropriate. Decay must have a stated time
basis independent of callback/block size and display refresh rate. Preserve
resolved channel identities rather than silently folding stereo channels,
and account for samples outside the displayed `-1` to `+1` range instead of
silently folding their mass into the endpoints. Missing coverage, unavailable
data, and unobserved time are not zero-valued samples. Normalization and pause/
discontinuity behavior should be made explicit with the complete feature.

**Time-frequency content.** Separately, allow a future `Sample` observation
presentation showing frequency content *over time*, initially through a
short-time Fourier transform (STFT) or, when justified, a wavelet transform.
The result should preserve the meaning of its time/frequency bins, channel
identity, window/hop or wavelet scale, magnitude/power and any logarithmic
frequency or dB grading. Time/frequency resolution and analysis latency are
real tradeoffs, not merely a canvas-rendering choice. This visualization is
distinct from amplitude density: it retains temporal spectral structure rather
than counting where scalar samples fall on the amplitude axis.

Both views should operate on truthful, bounded observation data using the
existing `Observations`, `GraphJit`/`GraphExecutor`, and background-query
ownership. A live Tick source and a random-access Tock/recorded source may
require different capture/query implementations; neither justifies doing a
large transform or growing a long-lived histogram in the audio callback.
They follow completion of the core observation and editor priorities in
[Vue Client Implementation Stages](./vue_client_implementation_stages.md).

## 3. Output production/retention determines the data path

| Resolved output | Observation source and execution |
| --- | --- |
| Tick / `ephemeral` | Live probe while the produced value is valid; optional bounded interval reduction or recent-history capture |
| Tick / `persisted` | Versioned random-access read of the already published recorded output for time-window queries; a live probe only if a *separate* live operation needs one |
| Tock / `ephemeral` | Exact, sparse on-demand background coverage evaluation with transaction-local materialization and bounded result projection |
| Tock / `persisted` | Versioned random-access query of a complete published data root; completion of an unpublished successor uses normal background transactions |

The author chooses output retention according to the computational/data-retention
contract. Expensive broad-dependency computations should generally publish
persisted outputs if their results need repeated random access, but persistence
can still have expensive initial completion and strictly retained storage costs.
The viewport must **never** change `OutputRetention` or implicitly promote an
ephemeral output to persisted. Observation-owned temporary results and optional
bounded, evictable derived-view caches are not authored persistence.

The existing random-access contract also permits background replay of eligible
Tick/ephemeral subgraphs when their dependencies are reproducible. This is not
an implicit recording guarantee for arbitrary non-replayable Tick output.
Observation supports only execution paths that are already semantically legal.

### Live probes

GraphJit may insert a probe at a producer/post-operation boundary where the
value remains valid, not blindly at the end of the whole root callback. A probe
must have prepared bounded memory and a nonblocking, allocation-free realtime
capture/handoff. Realtime publication never performs network I/O, blocking
serialization, or unsynchronized reads of mutable state from another thread.

An operation may capture the latest value, reduce over an interval, or retain a
bounded recent window; the framework should not force all `Sample` observers
into a peak accumulator. A cheap sample maximum is not an inter-sample *true
peak* estimator. The latter can be an explicit DSP node or separately requested
operation if ever justified. Live change suppression is operation-specific:
replaceable snapshots may be coalesced, but event histories or window statistics
must preserve the meaning they promise.

The combined active interests of all clients may change GraphJit's compiled
capture plan. Use incremental compilation and preserve unaffected state storage,
stack/scratch allocation, scheduling, and native-region specialization whenever
possible. An otherwise unused value may require materialization or production;
change only the affected realization. Observation-only changes must use safe,
ordered `GraphExecutor` generation activation, **not** an independently competing
compilation/cutover pipeline. Empty demand should remove unnecessary realtime
capture work; viewport hysteresis reduces plan churn. Whether the plan is a
separate overlay or part of `RealtimeRootPlan` is an implementation choice.
Structural builds compile with the same accepted executable live-probe
requirements, so an edit does not drop live probes; see
[Accepted observation requirements are realization inputs](#accepted-observation-requirements-are-realization-inputs).

### Background window queries

A range observation names one port, a finite global timeline window or exact
requested positions, an operation, and a bounded resolution/quality request.
For `tock/ephemeral`, its demand is intersected with exact `IndexCoverage` and
batched through the existing reverse-coverage/evaluation transaction. Any needed
projection of ephemeral data occurs while its transaction-local views remain
valid, yielding an independently owned **bounded** result; the client never
pins arbitrary Tock scratch or receives a dangling storage pointer.

For published `tock/persisted` and Tick/persisted outputs, query the relevant
subset of a pinned immutable `PersistedDataStore` version. A Tock/persisted
candidate that is not complete must not be displayed as if it were published;
completion and atomic publication retain their existing full-output-coverage
contract. No observation subsystem duplicates persisted records or invents a
partially committed persistent output.

A range observation on an input demands the input's resolved binding through the
same evaluation transaction. Coverage observations preserve Region ranges and
uncovered positions. Sequential observations use a coherent retained or
replayable binding and apply the same fallback as the receiver. When covered
data is unavailable, leave the affected values unavailable.

A presentation can call `usePortObservation` with a reactive request that has a
`source`, an `operation`, and a `window` for a range query. Set `source.kind` to
`input` to read the receiving port. For a multi-channel port, specify a
`source.channel`. Each presentation owns its subscriptions and releases them
when its Vue scope ends.

Sequential Sample inputs can report live values where the Tick callback receives
the binding. Windows can read concrete random-access bindings and replay-backed
Sequential bindings.

Public Coverage inputs remain structural aliases in `NodeBundle`. The
compiler retains their resolved sources, channel projections, and read delays
in `PublicCoverageInputView`. A window query requests the source data through
the existing background transaction and projects it at the public port. The
projection preserves Region identity and precedes routing to internal consumers.
The view adds no scheduled callback or playback storage. An unconnected port
returns an empty window. A route with no recorded or replayable source reports
that the data are unavailable.

## 4. Viewport resolution and truthful results

A waveform for a long song must not transfer the whole underlying sample stream
when only a few hundred display columns are needed. Vue requests a time window,
bounded result size, and supported quality/operation; the backend returns a
bounded representation with its actual coverage and provenance.

- **Representative/sparse sampling** asks for a limited set of positions. For
  pointwise Tock producers this can avoid most work, but it is an approximation:
  peaks between selected samples can disappear. It may not cheaply satisfy a
  node with broad upstream dependencies. Do not promise universal cheapness.
- **Interval envelope** (e.g. min/max per column) preserves extrema according to
  its declared fidelity, but an exact envelope needs every contributing sample
  or suitable prior summaries. A future multiresolution derived cache may
  accelerate persisted range queries; it remains optional, bounded and evictable.
- **Event windows** return exact bounded occurrences where requested, or an
  explicitly lossy count/activity projection at lower resolution. Sparse
  point-sampling/interpolation is not a substitute for event enumeration.

Results distinguish **absent coverage**, **covered neutral/zero data**, and
**data not yet available for the selected version**. Never interpolate across
missing `IndexCoverage`, invent samples or events, or display a partially
completed persisted candidate as current data. Operations specify whether their
result is approximate or exact, and report the source version/covered regions
needed to interpret it.

Display pixel count, publication rate, history length, and source operation are
not equivalent implementation changes. A resized view may need only a different
background reduction; adding or removing an executable probe may require
incremental GraphJit work. The system chooses the minimum necessary changes.

## 5. `Observations` application-module ownership

Introduce **one** application module named `Observations` for the Vue-driven
observation feature. It is not currently implemented and does not require a
thread simply because it is a module. It owns:

- connection-scoped/per-presentation-scope **desired observation interest sets**;
- validated semantic sources, type capabilities, observation requests, and
  aggregate requirements across all interested clients;
- subscription reconciliation, lease/expiry, disconnection cleanup, and
  generation-safe result routing and invalidation (the generation's own
  compilation has already resolved compatible probes; see
  [Accepted observation requirements are realization inputs](#accepted-observation-requirements-are-realization-inputs));
- coordination of live-probe requirements and versioned background window
  queries, including cancellation/discard of obsolete viewport results;
- shared/capped observation work where valid, result collection and change
  detection, per-client bounded delivery batching, and optional derived caches.

It does **not** own Vue components, presentation registration, authored graph
state, semantic node-message routing, type-specific DSP code, `NodeStorage`,
canonical persisted outputs, GraphJit compilation, or executor cutover. It
expresses aggregate requirements to existing owners. The graph-generation
coordinator remains `ProjectGraph`; `GraphJit` and `GraphExecutor` preserve their
existing compilation and accepted-transition ownership. Background coverage
requests join `GraphExecutor`'s background owner, not a second evaluation worker
or a caller-driven direct `tock_coverage()` path.

Requests and notification deliveries obey the application event-tree rule.
Do not cause the same incoming `SocketRpcServer` request to enter that module
again through a synchronous notification edge. Batch/asynchronous publication
is a distinct cause; a worker or scheduler is optional until demonstrated
necessary by actual load.

### Accepted observation requirements are realization inputs

_Accepted design decision, 2026-11-12. The superseded after-cutover rebinding
wording is preserved in [historical documentation](./historical/vue_observation_rebinding_before_realization_input_invariant_2026-11-12.md)._

**Every structural graph build includes the accepted executable live-probe
requirements.** Compatible probes are resolved and incorporated into the same
compilation and staged generation as the structural edit. Restoring those probes
must not require a second compilation after cutover. This applies to node and
connection edits, package refresh, and replay. Whether a change is represented
by an updated `RealtimeRootPlan` or another coordinated observation plan remains
an implementation choice.

`Observations` owns the per-client desired sets and their aggregation.
`ProjectGraph` retains the last **accepted aggregate executable requirements**
for compilation. These identify semantic sources (`NodeAddress` and an input, output,
channel, or supported Tick `State` field) and the capture operations needed,
not client subscriptions or generation-local indices. Non-executable window
queries, client delivery cadence, and purely presentational resolution settings
do not enter that snapshot. `ProjectGraph` must not independently modify its
accepted requirements; it resolves their availability against each new graph.

Newly settled on 2026-11-21. `ProjectGraph` coordinates each change in executable
requirements with `GraphExecutor`. If running code exposes the requested
sources, the executor can publish observation bindings at a Tick block
boundary without compiling. The bindings can reuse sample capture points
and read resident Tick State fields. Removing a binding stops its capture.
The compiled capture point can remain available for a later request.

If the running code lacks a required capture point, `GraphJit` compiles a
candidate. The executor can publish the candidate's steady Tick code in the
active generation when it can use the same state layout and invocation
workspace. Otherwise, the executor prepares a successor and transfers the
state of the same configured graph. If preparation cannot preserve that
state, the active generation stays in use. This applies to Tick State and
retained port data. A preview can start with a fresh Tock cache under the
[Tock State contract](./graph_jit_direction.md#domain-owned-state-realtime-packed-storage-or-background-sidecar).

`ProjectGraph` reports preparation progress by attempt. An attempt can finish
by attaching bindings, replacing code in the active generation, staging a
successor, or producing an error. Predictive compilation has no execution
generation. `ProjectGraph` assigns one when a compiled candidate needs
`GraphExecutor` to prepare an ordered cutover. A configured snapshot can be
published before it has an accepted execution generation.

Aggregate-interest equality and coalescing avoid redundant builds. If
requirements change during compilation, the latest request can require
another compile. The one-compile guarantee still applies to a structural
edit with unchanged accepted requirements. See the [former compilation
rule](./historical/observation_compilation_path_2026-11-21.md).

**All compilation paths must use a coherent requirement snapshot.** This includes
`ProjectGraph::control_compile_worker()` for hover/dynamic-control and settled
constant specializations, not just structural root builds. A result prepared
with older executable probe requirements must not subsequently stage or
activate and replace a newer probe plan. Pending, in-flight, and ready control
code must be checked against the accepted requirements when used, even if the
structural `project_generation` has not changed. The implementation may use an
independent requirement revision or advance an existing generation identity,
but a check of structural `project_generation` alone is insufficient when
observation-only changes do not advance it. Requirements that affect only
query results or delivery cadence do not invalidate compiled code.

**Staging and cutover differ.** A root build returns after staging a successor
in `GraphExecutor`; activation occurs at a later safe boundary. The coordinated
outcome must provide enough generation identity and **aggregate source**
resolution information for `Observations` to prepare routing and identify
unavailable sources. It need not return per-client subscription bindings:
`Observations` maps source resolutions to the interests it owns. A failure to
stage a successor must not invalidate valid bindings of the current generation.

The application-module event-tree rule still applies. When `Observations`
requests compilation, the outcome returns on that request path, without a
notification re-entering `Observations` for the same cause. For a structural
edit, `ProjectGraph` may inform `Observations` once after staging as another
child of the root build. This notification is a **staging outcome**, not an
activation event. The exact result record and bridge arrangement are left to
implementation.

**Continuity across cutover.** Compatible interests retain their logical
identities. The compiled successor owns its own probe bindings; `Observations`
accepts data only from a valid generation and prevents older frames from
replacing newer values. Vue may retain its last displayed value until a valid
replacement arrives, avoiding a blank meter while bindings change. Removed or
incompatible sources must be invalidated when the applicable generation becomes
active, not prematurely when it is merely staged. No observation may retain
pointers into a reclaimed generation.

An unchanged lane and compatible meter source keep their presentation instance
and peak/RMS motion across executable replacement. A new graph or activation
number alone must not reload the meter, blank it, or pulse its opacity. Waiting
for executable preparation is distinct from confirmed source unavailability.
While waiting, Vue can continue presenting its copied measurement history, with
status/accessibility information identifying that it awaits a new measurement.
Removed sources, changed node definitions, and incompatible channel/result
contracts invalidate that history; an actual unavailable result still clears it.

**Routing needs actual activation identity.** The identity used to validate
collected values and source invalidations must distinguish the compiled
execution/probe bindings that actually became active, including control-code
replacements within one structural project generation. Routing needs the
activation identity in addition to the execution generation. The observation service
must also be able to learn that activation occurred **without receiving a
nonempty observation update**: a successor can remove the last observable
source, leaving no value to publish. Activation status may accompany an empty
collection or use another executor-owned, non-reentrant signal; no particular
transport or callback is prescribed. Staged source resolutions remain
provisional until that activation is established.

## 6. Vue interest and transport semantics

Vue owns the selection of the generic/specialized component and computes
observation interests **reactively** from viewport, mode, geometry, and
interactions. Component mounting is not an individual network subscription.
One host-owned scope reconciles batched desired sets across components:

Here, a **presentation scope** means one interest-owning UI surface, typically
a graph canvas/viewport or an independent panel, scoped beneath one authenticated
client connection. It is **not** an individual node component or the node's
visible rectangle. Many node components contribute requirements to one scope;
a client can own multiple scopes. The frontend replaces/reconciles each scope's
full desired set; the server merges effective interests across all scopes and
clients. Scope destruction, disconnect, and lease expiry release its interests.

- **visible:** full supported quality appropriate to that view;
- **nearby/predicted:** cheaper, lower-rate interests or metadata prefetch;
- **inactive:** no producer work after a bounded deactivation grace period.

Overscan, hysteresis, predictive prefetch, and batched scope replacement avoid
subscribe/unsubscribe and recompilation on every pan/zoom frame. Effective
producer work is shared across compatible clients. A scope replacement
implicitly releases absent interests; explicit unsubscribe is optional.
Connection disconnection and expiring leases guarantee cleanup even if Vue
does not send a final release. An interest is associated with an authenticated
connection, not an arbitrarily trusted client-supplied owner ID.

The [shared presentation performance rules](./node_presentation_and_manual_controls_direction.md#shared-presentation-performance)
also apply to custom lanes. The observation service owns the request windows
and selects the result for each span of the visible range. A completed window
with empty coverage replaces any cached preview in that span. A positional
event source can require the precise visible window. An indexed source can
reuse a padded window across nearby views.

When a lane leaves the visible tier, the client can keep recent ready results
in a bounded display cache. Retiring an interest releases server demand.
When the lane returns, a finer cached result can cover the same span while a
replacement is pending. A source or sample-rate change invalidates incompatible
data. Active readings remain available when the cache evicts their history.

Maximum live publication rate, nearby-tier cadence, overscan distance,
debounce/hysteresis, deactivation grace time, and lease/heartbeat timeout are
bounded **tuning parameters**, not architectural constants. Choose and measure
initial defaults in the working Vue host and server; do not hardcode invented
protocol guarantees or trigger plan rebuilds for changes that affect only
result reduction/network cadence.

`Observations` owns the server's collection deadline, window retry
intervals, and scope leases. Its worker waits for the next deadline or an
execution activation. Activation can rebind windows early. Delivery still
follows the same deadline. The runtime reports activation as part of
execution supervision. The socket writes eligible observation batches and
keeps the newest unsent batch for each client. The frontend schedules its
own viewport interest changes.

**Live updates** are maximum-rate, replaceable/coalesced, per-client batches.
If a value hasn't changed according to its operation semantics, no update is
required. A slow client may skip superseded observation frames; disposable
telemetry must not consume the ordered project revision/replay log or trigger
its normal slow-reliable-client failure behavior.

**Window results** are bounded versioned query results, not periodic meter
notifications. An update/invalidation can target an intersecting covered range
when underlying published data changes. A result that is still valid but no
longer requested because the viewport moved should be discarded, without
pretending that the graph data became invalid. Lossless event histories require
sequences/gap recovery or reliable query semantics; they must not be silently
coalesced like a latest-value meter.

The external boundary reuses existing semantic `NodeAddress` plus resolved
output/channel identity, with implementation-local capture identities and
versions kept internal. Replies/results carry sufficient generation and
`(semantic_version, data_version)` provenance (where applicable), actual
coverage and request identity to reject stale deliveries. A compatible semantic
source keeps its logical subscription identity across a generation change: the
successor's compilation resolves its probe, and `Observations` routes results
from the active generation to the same subscription. Incompatible or removed
sources are invalidated when the relevant generation activates. Never retain
pointers into reclaimed generations. See
[Accepted observation requirements are realization inputs](#accepted-observation-requirements-are-realization-inputs).

`SocketRpcServer` transports these messages and applies connection/security
policy; it does not own observation interests or observation DSP computation.
Do not add speculative per-source RPC routes: settle the concrete batched
request/notification protocol from the first working Vue requirements.

## 7. Work priority, backpressure, and reuse

Observation must not starve correct/timely playback, graph transitions, or
completion/publication work that the runtime already requires. Prioritize
playback-critical background materialization above visible user window queries,
visible above prefetched interests, and discard/deprioritize obsolete requests
where safe. Use budgets for work, memory, payload size, and per-client transfer;
expensive views may be incomplete or deferred rather than block audio.
Cancellation must not interrupt a transaction in a way that violates persisted
publication or background-generation ownership.

Compatible interests can share one producer, query, or type-specific reduction.
Persisted derived summaries may eventually be cached by source, version/coverage,
operation, and parameters; their eviction never changes the underlying
`OutputRetention` contract. The first implementation need not invent a
multiresolution cache to prove correctness.

## 8. Implementation boundaries still intentionally open

Design now fixes ownership and semantic guarantees, **not** the new wire routes,
Vue composable names, compiler descriptor layouts, task/thread topology,
encoding, or the exact first operation IDs and parameters. The first Vue
implementation should prove at least one reflected Tick scalar, one real live
output (e.g. `Sample` level), and one bounded persisted or sparse Tock output
window, while validating safe capture lifetimes, ordered generations,
background priority, truthful coverage, and batched multi-client delivery.

Defer field-level serializer overrides, Tock `State` observation, arbitrary
authored observation callbacks, elaborate derived caches, and broad custom
spectrum or event operation catalogs until demonstrated needs.
