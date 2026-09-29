# Outstanding Implementation Fronts

_Status: coordination map for design work that is compatible with the current direction but has not yet fully landed in the implementation._

This document groups the remaining work by implementation ownership rather than by
which design document first introduced it. The goal is to make the outstanding work
parallelizable: one person or team should be able to own one front, define the missing
local details inside that boundary, and integrate through explicit contracts with the
other fronts.

This is not a replacement for the detailed design documents. Those documents remain
normative for semantics. This document answers a different question: **where should
implementation ownership be cut?**

A front may contain a mixture of completely unimplemented work and partially landed
infrastructure. "Outstanding" means the front is not complete enough to remove from
the implementation roadmap.

## Coordination principles

The fronts below follow five rules:

1. **Separate semantic contracts from storage representations.** Port realization,
   random-access storage, capture, and revision migration interact, but they should not
   be one implementation project.
2. **Keep authored callback shape behind node traits.** GraphJit should consume
   normalized operations and solved realizations rather than rediscovering source-level
   callback choices.
3. **Keep project intent distinct from derived runtime state.** Configuration source,
   connections, semantic node events, and persistence belong to the control plane;
   GraphJit and executor runtime artifacts do not.
4. **Land correctness before optimization.** Revision continuity, transactionality,
   and semantic delivery must not depend on a later batching/fusion/PGO pass.
5. **Prefer explicit integration contracts over shared ownership.** A team may add a
   temporary adapter to the current representation rather than taking ownership of a
   neighboring front merely to make progress.

## Front 1: port constraints and realization

**Primary ownership:** `constrain_ports()`, graph-wide port-property solving, and the
immutable resolved realization consumed by declaration, storage planning, scheduling,
and lowering.

This front owns:

- `ConstrainPortsContext<Node>`;
- dynamic continuous-value `size()` constraints;
- exact `pace()` constraints and pace-domain solving;
- Sequential-input `history()` constraints;
- Tick-output `latency()` constraints;
- Coverage ordering/sortedness constraints;
- finite registered value-type alternatives and value-type equality constraints;
- connection-generated constraints between producer and consumer ports;
- conflict and unresolved-property diagnostics;
- effective local sample-rate domains after pace resolution;
- per-port Tick block extents;
- Sequential `block_extended()` realization;
- the initial continuous-value registry, including `Sample`, `GlobalIndex` and
  `FFTBlock`;
- realization-time selection of one concrete type for multi-type ports before storage
  planning/lowering;
- graph-resolved FFT dimensions and storage-size information;
- the planned type-safe sample/event port-config representation;
- Sequential `neutral_value` / `combine_values` semantics where they affect
  realization and delivery legality.

Coverage ordering is a solved graph property, not only a static producer flag. A node
may make the ordering guarantee of one output depend on the ordering guarantee of an
input:

```cpp
void MergeLikeNode::constrain_ports(
    ConstrainPortsContext<MergeLikeNode>& ctx) const
{
    ctx.output<"out">().ordering() = ctx.input<"in">().ordering();
}
```

Connections then propagate the required/guaranteed ordering relationship through the
graph. A downstream sorted-Coverage requirement can therefore become a transitive
constraint over the nodes feeding it. The solver may satisfy that requirement through
naturally sorted producers and k-way descriptor merging, or require an explicit
ordering realization where an upstream contribution is not guaranteed sorted. Exact
constraint-proxy spelling is owned by this front; the semantic requirement is not.

The output contract of this front is an immutable **resolved port realization**. Other
fronts should consume that object instead of each independently resolving size, pace,
history, latency, or ordering.

**Can start independently:** yes.

**Important consumers:** fronts 2, 3, 4, 6, 7, and 8.

Detailed direction:
[Graph JIT Direction](./graph_jit_direction.md),
[Random-Access Port Data Direction](./random_access_port_data_direction.md), and
[Sequential Port Storage And Connection Planning](./sequential_port_storage_planning.md).

## Front 2: batched primitive callbacks and scheduling

**Primary ownership:** first-class scalar/batched node callback normalization and the
scheduler's ability to exploit same-implementation batches.

This front owns:

- authored `tick()`, `tick_block()`, and `tick_block_batch()` normalization;
- authored `skip_block()` and `skip_block_batch()` normalization;
- authored `tock_coverage()` and `tock_coverage_batch()` normalization;
- forward/reverse propagation scalar and batch normalization;
- iterable batch contexts with `size()`, `operator[]`, `begin()`, and `end()`;
- singleton-batch adaptation when a node only defines a batch callback;
- sample-major/lane-minor generated batching for `tick()`;
- ordinary lane-loop generated batching for scalar block callbacks;
- compiler/reflection anchors for every normalized scalar and batch operation;
- batch-compatibility keys;
- dependency-aware grouping of ready nodes from the same batch class;
- virtual-node pre-grouping as an acceleration hint rather than a semantic boundary;
- scalar fallback when batching makes the surrounding schedule worse;
- batched background propagation and Tock ready-frontier execution.

The node-trait API is the only place that branches on which authored callback exists.
GraphJit sees `do_*` and `do_*_batch` operations unconditionally.

Front 1 may later enrich the batch-compatibility key with solved size/pace/history/
latency/ordering facts. That should not prevent this front from landing against the
current realization identity first.

**Can start independently:** yes.

Detailed direction:
[Batched Node Callback Direction](./batched_node_callbacks_direction.md).

## Front 3: Region/Coverage random-access representation

**Primary ownership:** the authored and runtime random-access data model after the
page-oriented checkpoint.

This front owns:

- first-class contiguous `Region` values;
- `Coverage` as a collection of possibly overlapping Regions;
- preservation of multiplicity/overlap rather than forced disjoint normalization;
- unordered and sorted Coverage representations;
- canonical sorted order and k-way descriptor merging;
- Region/Coverage Tock inputs and outputs;
- Region/Coverage Random Access inputs;
- bounded Coverage-to-Sequential reduction;
- ephemeral materialization ownership;
- persisted Region/Coverage publication and versioning;
- replacement of fixed-width page assumptions in the authored/runtime storage model.

This front consumes solved ordering requirements from front 1. It owns **how** sorted
Coverage is represented or materialized, not **whether** a particular graph edge
requires sortedness.

**Can start mostly independently:** yes; use adapters to the current page store if
needed while front 1 lands.

Detailed direction:
[Random-Access Port Data Direction](./random_access_port_data_direction.md) and
[Coverage And Background Evaluation](./coverage_and_background_evaluation.md).

## Front 4: complete background-to-Tick delivery

**Primary ownership:** execute every already-planned ordinary background delivery at
Tick time.

This front owns:

- sequential persisted/materialized snapshot playback into Tick inputs;
- neutral values for genuinely absent Sequential data;
- background-only sample delivery;
- mixed Tick/background sample fanout;
- background-only event delivery;
- mixed Tick/background event delivery;
- disconnected/default behavior for those dynamic bindings;
- removal of the current capability guards that reject those cases.

This front does not own capture, project persistence, or graph-revision state
migration.

**Can start now:** yes. Prefer consuming the target Region/Coverage representation from
front 3 when practical, but do not merge the ownership of the two fronts.

Detailed direction:
[Coverage And Background Evaluation](./coverage_and_background_evaluation.md).

## Front 5: realtime-produced queues and explicit recording

**Primary ownership:** realtime-safe production of persisted/background-visible data
from realtime execution and its ownership transfer to background work.

This front owns:

- producer-specific provisioned SPSC queues backed by power-of-two blocks;
- `AsyncCapacityManager` provisioning/reclamation and producer `C/L/H/G` policy;
- direct sample/event production into provisioned blocks where layout permits;
- cheap complete-chain publication at realtime pass boundaries;
- Tick-persisted output handoff;
- explicit recorder handoff with fixed untouched/write/`write_void()` semantics;
- independent finite-prefix pinning by `BackgroundGraphExecutor`;
- publication into the canonical random-access persistence representation; and
- release/reclamation of completed prefixes after successful domain commit.

There is intentionally no atomic snapshot across different producer queues. If a
future semantic requirement needs one, stop and design that requirement explicitly.

Prefer integrating with the Region/Coverage target of front 3 rather than deepening a
page-only API that is already planned for replacement.

**Dependencies:** front 4 for the complete consumer side; front 3 is strongly preferred
before the durable publication layer is finalized.

## Front 6: graph-revision state continuity

**Primary ownership:** preserving semantically valid node/port state when one compiled
graph generation is replaced by another.

This front owns:

- stable matching of concrete semantic nodes and ports across revisions;
- Sequential history continuity;
- Tick-output latency/revision-window continuity;
- rewiring and fan-in/fanout changes;
- value-size and pace changes after front 1 lands;
- temporary transition realizations when old and new storage cannot be represented by
  one steady-state layout;
- transition expiry horizons and switch to the final realization;
- compatible persisted random-access rebinding;
- paired realtime/background generation identity and stable route/destination identity;
- producer/destination disappearance semantics for queued recording/persisted data;
- ordered old-generation drain + prepared background migration across realtime cutover;
- generation-compatible persisted-state publication; and
- generation reconciliation and stale-generation rejection/reclamation rules.

This is correctness work, not an optimization pass.

**Dependencies:** current executor infrastructure; consumes front 1's solved
realization and front 3's final persistence representation as those land.

## Front 7: remaining GraphJit and node execution semantics

**Primary ownership:** legal authored node/layout constructs that still have explicit
lowering guards or incomplete runtime scheduling semantics.

This front owns, among other remaining gaps:

- nested declarations;
- declared arrays/auxiliary/compiler-owned regions;
- remaining external-storage layout forms;
- activity/TTL semantics;
- actual scheduling of skip callbacks rather than callback discovery alone;
- detach/event lowering edge cases;
- tiled/subgraph connection-analysis gaps;
- moving background-only `TockState` out of canonical realtime `NodeStorage`;
- normalized block replayability based on semantic requirements rather than whether
  the source author happened to write `tick()`.

Replayability should eventually be decided from the normalized `do_tick_block()` plus
resolved state/history/latency/pace facts. A native `tick_block()` or batch-only node
may therefore be replayable at complete legal invocation quanta. `tick()` remains a
first-class optimized authoring form because its generated batch adapter can expose
cross-node SIMD opportunities.

**Dependencies:** front 1 for final realization predicates; front 2 for normalized
batch operations. Other sub-items can proceed earlier.

## Front 8: application execution and system-audio integration

**Primary ownership:** make the new execution architecture the application's normal
runtime path.

This front owns:

- construction and lifetime of `RealtimeGraphExecutor` and `BackgroundGraphExecutor`;
- direct `ProjectGraph` bridges that stage `BackgroundGraphExecutor` first and
  `RealtimeGraphExecutor` second after successful `GraphJit`;
- paired-generation staging with all cutover allocation completed off realtime;
- realtime-authoritative pass-boundary cutover: final old-generation chain publication,
  allocation-free ordered cutover publication to background, then realtime swap;
- background completion of already-selected old-generation work, closed old-generation
  queue drain, and prepared migration before consuming new-generation queues;
- preservation of multiple ordered cutovers while background lags;
- realtime compiled-generation state migration and pass-boundary activation;
- audio callback routing through `RealtimeGraphExecutor`;
- `BackgroundGraphExecutor` worker/evaluation lifecycle and independently pinned
  producer queues;
- `AsyncCapacityManager`-style provisioning/reclamation for producer-specific
  power-of-two queue blocks;
- realtime-to-background produced-chain publication and background-to-realtime
  immutable persisted-state publication;
- retired generation/snapshot/queue-block reclamation scheduling;
- executor/capacity-manager shutdown and application error propagation;
- stable logical system-audio device bindings;
- ordinary project graph input/output nodes using those bindings;
- silence/discard behavior when a persisted logical device cannot currently resolve;
- device-clock buffering/resampling boundaries without mutating project topology.

Automatic graph-node creation for every detected hardware device remains optional
convenience behavior, not a prerequisite for this front.

**Dependencies:** front 4 is the near-term execution prerequisite; enough of fronts 6
and 7 must be present before this becomes the sole production path.

## Front 9: restricted configuration-expression language

**Primary ownership:** parse persisted/project-authored configuration source into owned
typed argument values without accepting arbitrary C++ execution.

The persisted syntax should remain C++-like because it needs natural typed literals,
aggregate construction, and nested initialization lists, but the accepted language is
intentionally much smaller than C++. This front owns the grammar, parser, type checking,
and evaluation model.

Required language capabilities include at least:

- scalar/string/enum literals as required by registered configuration signatures;
- nested braced initialization lists and aggregate/value construction;
- lists/arrays and other registered container/value structures as construction
  arguments;
- ordinary numerical arithmetic such as `+`, `-`, `*`, `/`, unary operations, and a
  small allow-listed set of pure mathematical functions;
- a small framework-provided reference/query vocabulary, potentially including forms
  such as `ref(user_authored_node_id)` and
  `select(virtual_node_name)[idx]...`;
- no arbitrary statements, mutation, loops, allocation APIs, filesystem/network
  access, arbitrary function calls, lambdas, templates, or general C++ execution;
- deterministic evaluation with no correctness dependence on how often an expression
  is evaluated or cached.

Exact grammar and spelling of the predefined functions are owned by this team. The
important contract is a deliberately restricted, auditable expression language rather
than handing arbitrary persisted source to Clang.

The evaluator must use the registered configuration signature/type information to
produce the same owned typed argument tuples already consumed by `NodeInstances`.
Provider-generated operations may still be used for clone/destroy/equality/hash and
for safe construction of provider-defined value types; that does not require compiling
arbitrary user-authored C++.

The expression/value representation should also support **argument-preserving node
replacement**. An `on_message` handler that replaces its own semantic node must be able
to inspect/reuse the predecessor's configured argument values and replace only selected
arguments rather than reconstructing the complete configuration from unrelated copied
captures. Lists and other structured argument values must remain ordinary first-class
values so module-node automation can evolve a dynamic set of construction arguments.

This front should expose a stable parsed/value representation that fronts 10 and 13 can
use for persistence and semantic replacement without duplicating parsers.

**Can start independently:** yes.

Detailed direction:
[Node Definitions And Instances Direction](./node_definitions_and_instances_direction.md),
[Project Graph Application Architecture](./project_graph_application_architecture.md),
and [Project File Serialization Direction](./project_file_serialization_direction.md).

## Front 10: generalized project persistence and graph mutation APIs

**Primary ownership:** the durable desired project graph and transport-neutral mutation
surface.

This front owns:

- stable project node IDs;
- definition ID plus restricted configuration-expression source;
- generalized node create/update/delete;
- complete `GraphConnections` persistence;
- structured `ProjectNodePortMatcher` persistence;
- connection create/update/delete;
- dangling matcher preservation;
- channel selector/type information;
- replay as one normalized project transaction;
- persistence collection from `NodeInstances` and `GraphConnections`;
- typed application requests/events for the same graph mutations;
- JSON-RPC adapters for those typed requests;
- richer/adversarial matcher-path coverage.

This front persists the expression source/AST/value request owned by front 9; it does
not own the expression parser or `NodeInstances` cache.

**Can start in parallel with front 9:** yes, using opaque source strings until the
restricted parser lands.

## Front 11: package artifact provenance and caching

**Primary ownership:** artifact ownership across built-in, common, and project-local
packages.

This front owns:

- explicit package provenance;
- overlapping-root deduplication;
- executable-owned built-in compiled artifacts;
- build/install-time prebuilding;
- installed executable artifact/cache locations;
- correct invalidation keys;
- shared common-package cache ownership;
- project-local build/cache ownership;
- avoiding per-project recompilation of unchanged built-ins/common packages.

It should preserve the existing `PackageWatcher -> PackageJit -> PackageDefinitions ->
NodeDefinitions` responsibility split.

**Can start independently:** yes.

## Front 12: scoped `BuilderSession` / `SubgraphClosure`

**Primary ownership:** same-session scoped graph construction after the current
build/configuration path has been measured and optimized.

This front owns:

- one mutable `BuilderSession` with scoped builders/views;
- zero-copy same-session nesting;
- `SubgraphClosure`;
- stable scoped identities;
- prevention of stale/free builder captures;
- live-reference ownership migration;
- preserving direct frozen `ConfiguredGraph` import;
- equivalent cache-hit/cache-miss semantics.

This front remains deliberately gated until GraphJit plus the two executor modules have become the
normal path and the current builder/configuration path has been profiled.

## Front 13: source introspection and semantic node interaction

**Primary ownership:** the typed semantic event/control layer inside the backend.

This front owns:

- making source introspection mandatory package infrastructure where required;
- validation and stable identity for declared `State`/`TockState` fields;
- member-pointer-based declaration validation;
- typed node-message handler discovery;
- generated event serializers/deserializers and frontend type metadata;
- stable `NodeAddress` and selector semantics;
- leaf and module-node handlers;
- ordered one-way node-to-node event dispatch;
- graph-revision-safe semantic addressing;
- node-local replace/reconfigure operations;
- exposing the predecessor node's current configuration arguments to a replacement
  handler and allowing selected arguments/structured subvalues to be replaced while
  preserving the rest;
- interaction journaling/commit integration without exposing realtime storage to the
  public handler context.

The actual configuration values and expression/value manipulation used by replacement
come from front 9. This front owns event semantics and replacement transactionality,
not the configuration grammar.

The live-observation/presentation telemetry API remains separate and may stay unresolved
until front 14 needs concrete capabilities.

## Front 14: presentation and persistent manual controls

**Primary ownership:** semantic presentation definitions and user-controlled dynamic
inputs.

This front owns:

- `NodePresentation` and framework default presentation;
- global display modes;
- authored/scoped semantic rebinding;
- presentation lifetime independent of frontend component lifetime;
- persistent manual values;
- participation policy;
- manual values as Tick/ephemeral inputs;
- scrub-only interpolation;
- dynamic-input sets;
- predictive JIT requests;
- constant/dynamic compatible code variants and stale-compile rejection;
- eventual live-observation capability/subscription APIs once concrete requirements are
  settled.

**Dependencies:** semantic identity/events from front 13; revision continuity from
front 6; later specialization benefits from front 12 and the mature executor.

## Front 15: RPC route layer and multi-client project session server

**Primary ownership:** transport/session infrastructure around typed application APIs.

This front owns:

- replacing hard-coded socket dispatch with a route/event registry where still needed;
- multiple simultaneous clients;
- per-client outbound queues/backpressure;
- ordered mutation execution;
- snapshots and monotonically increasing project revisions;
- subscription/replay;
- expected-revision mutation requests;
- reconnect/resynchronization;
- local server discovery/election;
- transport-independent endpoint semantics;
- client identities/capabilities;
- optional remote-audio lease/WebRTC relay work.

This front consumes project mutations from front 10 and semantic node events from front
13. It should not invent domain-specific graph semantics in the transport layer.

## Front 16: measurement-driven optimization and optional PGO

**Primary ownership:** optimization after the correctness architecture is stable.

Candidate work includes:

- batch-size/cost modeling beyond front 2's correctness-capable scheduler;
- SIMD profitability heuristics;
- callback/kernel fusion;
- storage-layout co-optimization for batched nodes;
- alias/materialization cost models;
- transient storage reuse/liveness improvements;
- immutable-value specialization;
- finer NodeInstances cache invalidation/reuse;
- measured builder/package reload optimization;
- runtime PGO experiments if profiling demonstrates a worthwhile target.

Runtime PGO remains an optional discovery direction rather than a prerequisite for the
architecture.

## Dependency summary

The main execution path is approximately:

```text
                           +--> 2 batched callbacks -----------------+
                           |                                         |
1 port realization --------+--> 3 Region/Coverage ----+              |
       |                   |                          |              |
       |                   +--> 7 GraphJit semantics |              |
       |                                              |              |
       +----------------------> 6 revision continuity|              |
                                                      |              |
existing background -------> 4 background->Tick -----+              |
                                |                     |              |
                                +--> 5 capture        |              |
                                                      v              v
                                           8 application/executor integration
                                                      |
                                                      v
                                               measured baseline
                                                      |
                                                      v
                                         12 scoped builder migration
```

The main control-plane path is approximately:

```text
9 restricted configuration language ---> 10 project persistence/mutations
             |                                      |
             +-----------------> 13 semantic events-+
                                      |
                                      v
                              14 presentation/controls

10 project APIs + 13 semantic events ---> 15 session/transport
```

Front 11 (package provenance/caching) can proceed largely independently. Front 16
should consume measurements from the integrated system rather than setting semantic
requirements for earlier fronts.

## Integration checkpoints

The fronts should integrate through a few explicit checkpoints rather than by waiting
for every neighboring project to finish:

1. **Resolved-port checkpoint:** front 1 publishes a stable realization object; fronts
   2/3/4/6/7 consume it.
2. **Normalized-callback checkpoint:** front 2 publishes unconditional scalar/batch
   compiler operations; GraphJit scheduling no longer inspects authored callback shape.
3. **Random-access checkpoint:** front 3 publishes the canonical Region/Coverage
   ownership/versioning interfaces; fronts 4/5/6 stop depending on authored page
   semantics.
4. **Execution checkpoint:** fronts 4/6/7 provide enough correctness for front 8 to make
   `RealtimeGraphExecutor` and `BackgroundGraphExecutor` the normal application path.
5. **Configuration-value checkpoint:** front 9 publishes the restricted parser plus
   typed/structured value representation; fronts 10/13 share it for persistence and
   replacement.
6. **Semantic-control checkpoint:** fronts 10/13 publish transport-independent typed
   APIs; front 15 provides multi-client/session transport without duplicating domain
   logic.

Temporary adapters are acceptable between checkpoints. Shared ownership of the same
semantic rule is not.
