# Output gain implementation

Status: active feature, 2026-11-17. The owner reports all 1,108 tests pass for
shared buffered storage and requests the next accepted feature: output gain.
Builds, tests, typechecks and profiling remain owner-run. Do not manipulate Git
staging or commits.

## Accepted contract

The owner clarifies on 2026-11-19 that gain is a port-owned `output * scale`
transform on authored outgoing connections. Direct output observations read the
node's original output, including meters and arbitrary-time waveform windows.
Input observations read the authored connections on that input through the
normal observation path. Changing gain does not mutate node state or retained
source bytes. The transform preserves coverage gaps, logical Region identity,
and source materialization validity. Consumers use a coherent selected gain.
[Earlier output observation contract](./historical/output_gain_before_connection_observation_2026-11-19.md).

The default node presentation uses the output knob's displayed gain to scale
its observed peak, RMS, and held peak. It applies the gain magnitude in Vue,
so the meter can preview a knob change while the executable catches up.

Region and Coverage output gains use a Tick multiplier when their authored
consumers only require realtime Sequential delivery. The multiplier preserves
uncovered positions so the destination can apply its existing fallback. If any
consumer needs background preparation, including through Tick replay, one Tock
multiplier supplies all branches from the same prepared gain. Observation
subscriptions do not participate in the choice.

This supersedes the owner's earlier same-session restriction to Sequential /
computed outputs. That restriction was discussed but not implemented or adopted
into normative repository text. The current intent includes all output shapes
and both computed/buffered realization, for value types with a defined scalar
gain operation. Existing normative restrictions on unsupported payload types
remain explicit; do not multiply arbitrary event or position payloads.

The owner explicitly selects `Sample` and `FFTBlock` outputs. Gain multiplies
the Sample value or both real/imaginary components of every FFT bin, across
every channel. Event outputs and `FractionalGlobalIndex` position outputs have
no gain control. This is an explicit type capability, not an inference from a
port having numeric-looking storage.

Gain defaults to exactly 1, has no minimum or maximum, and belongs to the output
port rather than each outgoing connection. Settled unity must produce the same
gain-free executable path as an absent gain operation: no multiply, runtime
gain load, branch or wrapper. Non-unity and interactive execution must reuse
the existing ordered control compilation/activation and undoable gesture model.
Hover prepares dynamic code; release commits and prepares settled code; leaving
the knob permits settlement activation. Preserve the existing mouse, pointer
lock, reset, numeric-entry and clipboard interactions.

Normative references:

- [Output gain controls](./vue_presentation_refinement_direction.md#3-output-gain-controls-and-graphjit-specialization).
- [GraphJit output gain specialization](./graph_jit_direction.md#output-gain-specialization-owner-requirement-2026-11-16).
- [Development workflow](./development_workflow.md).

## Implementation order and current findings

1. Verify control identity, project history/persistence, configured interfaces,
   compiler-owned execution stages, specialization and observation paths.
2. Define explicit gain capability/scaling behavior with the registered value
   extensions. Keep event and coordinate semantics explicit.
3. Extend output-control identity and publication through project edits, RPC,
   immutable configured snapshots and live gestures. A numeric control must not
   masquerade as an authored input port.
4. Implement the output transform across live execution, replay/background reads
   and observations. Retain source backing; apply gain before consumer-specific
   fan-out/conversion. Specialize unity away. Preserve feedback, history,
   multichannel values, module boundaries and safe graph/state migration.
5. Expose the gain through output-port controls in the default Vue presentation,
   using the existing knob interactions and history model.
6. Add semantic, ownership and emitted-code regressions once the implementation
   shape is established. Have the owner run required verification; consolidate
   only after those results.

Current source observation: `ManualControlId` names an input, and
`ConfiguredManualControl::input` resolves a Sequential Sample input. Existing
manual-control lowering rejects non-Tick and non-Sequential inputs. Output gain
therefore needs a real output-transform representation and deliberate control
versioning, rather than relabeling that input binding or mutating published
output storage. Background evaluations and retained observations must select
the same intended gain snapshot without allowing live target updates to alter
an already selected transaction.

The owner selects prepare-before-activation for gain changes requiring Tock
work: retain the previous ready gain/control snapshot until the replacement
window is prepared. Realtime branches must not advance independently while
their background-dependent branches still consume an older gain. Pure realtime
paths can use the ordinary prepared dynamic control path without waiting for
unrelated background work.

For that background-dependent case the owner explicitly accepts a direct gain
switch when the prepared snapshot becomes ready. A shared sample-by-sample ramp
across Tock and realtime is not required. Realtime-only gains retain the existing
manual-control interpolation. This removes the need to predict an audio-thread
interpolation origin or scheduled ramp in a background transaction.

No output-gain source implementation has been changed yet. The immediate next
work is to resolve the shared transform and snapshot boundaries above before
choosing compiler details. Report any additional architectural friction or
unsettled semantic decision instead of silently narrowing this feature.

One playback policy is pending: live gain on a source feeding Tock can require
new background results. The owner has been asked whether playback should retain
the previous ready control snapshot until new preparation completes, or let the
realtime branch advance while background results lag. Do not choose mixed
control versions implicitly. Source inspection shows that `ManualControlLiveTarget`
currently publishes atomic values without a transaction version, whereas
`BackgroundTransactionInputs` selects graph/data versions. This boundary must be
resolved as part of the complete feature.

## Source checkpoint, 2026-11-17 (unfinished, not verified)

The earlier no-source-change and pending-playback paragraphs above describe the
initial checkpoint. They are superseded by the owner's prepare-before-activation
and direct-switch decisions recorded above.

The working tree now extends control identity, persistence, RPC, configured
interfaces, gestures and output-port knobs. GraphJit inserts private output-gain
stages only for non-unity or interactive gains. Tick stages use the existing
Sequential control interpolation; Tock stages retain logical Regions and scale
only demanded windows. Source buffers remain upstream of these stages. Output
observations select the gained view. Background-dependent gains use immutable
prepared graph replacements instead of changing the audio thread's atomic target.

This remains one unfinished feature. Remaining work is to inspect history and
migration boundaries, add semantic and ownership regressions, review the whole
change, and ask the owner to build and run tests. No build, test, typecheck or
profiling run has been performed by the agent for this feature.

## Friction: buffered Tick gain cannot be another replayable Tick node

_Observed in source on 2026-11-17; no runtime verification has been run._

The first source pass assumes that a compiler-private Tick node can represent
both realtime output scaling and a retroactive view of a buffered Tick output.
That assumption is incorrect for the background view.

`expand_output_gains()` currently creates a Tick gain stage with
`TickReplayMode::full`. Its original source retains its buffered bytes, while
the stage output is computed. However,
`BackgroundPropagationWorkspace::replay_full_forward_coverage()` publishes
full-circle output coverage, and `publish_output_coverage()` enforces full
coverage for `BackgroundPortPlan::replayed_tick_output`. This is intentional for
actual replayable Tick nodes: Sequential input fallback gives them a value at
every position. A gain view of a recording has a different obligation: its
coverage must remain exactly the coverage of the recording. Unrecorded positions
must remain absent, rather than becoming covered zeroes. Reclassifying ordinary
Tick replay as sparse would contradict the accepted execution model.

The new buffered-gain regression checks the old recorded values, restoration of
raw values at unity, and absence beyond the recorded window. The current source
has a known semantic defect at that last boundary; the feature is not ready for
the owner to verify as a completed implementation. Other newly added tests are
also unbuilt and unrun.

Reach: private gain expansion, background topology and callback selection,
prepared read views, replay consumers, recording observations, and migration of
these derived views. Project control identity, persistence, gestures, frontend
knobs and type-owned Sample/FFTBlock scaling can retain their current shape.

Recommendation: give the compiler an explicit output-transform record and lower
it separately for realtime and background use. Realtime lowering may fuse a
multiply into a specialized kernel or use a private scaling callback. Background
lowering must use the original output's logical Regions and retained bytes,
applying the selected gain when preparing/read-projecting values. It must not
acquire Tick replay coverage or mutate raw recording storage. Unity removes the
transform in both lowerings. This can use bounded ordinary allocations and the
existing shared backing; no new virtual-memory system or public Tick output
shape is required.

A separate Tock projection paired with realtime scaling is another possible
lowering, but it still needs deliberate background routing so downstream replay
cannot accidentally traverse the full-coverage Tick gain stage. Do not add a
special exception that makes ordinary replayed Tick outputs sparse, or route
current realtime samples through a delayed background recording read.

Next dependency: settle that compiler representation, replace the incomplete
buffered Tick path, then finish background-dependent activation and semantic
regressions before asking the owner for builds/tests. No Git staging or commit
operations have been performed.

## Owner correction: input-union replay, 2026-11-17

The owner points out that `input_union` replay preserves upstream coverage.
The previous friction report describes the current full-only implementation;
it does not establish that a multiplication-node implementation is inherently
unsuitable. With input-union background replay, a multiplication node whose
only connected input is the signal can preserve that signal's sparse coverage,
while its open gain input supplies its manual/default scalar.

Current source discrepancy: `node/tick_replay_mode.h` contains only `disabled`
and `full`; background replay publishes full coverage. The November 16
superseding documentation also describes removal of the old input-union/full
coupling. Reconcile that migration with the owner's current clarification before
changing replay semantics or introducing a separate output-transform framework.
Live Sequential execution being dense does not by itself establish that
background replay must discard input-union coverage.

The immediate design question is whether `input_union` is to remain available
for background replay independently of dense live Tick execution. The earlier
categorical claim against replayable multiplication, and the resulting need for
a new transform abstraction, are withdrawn pending that reconciliation.

## Recovered intent and cleanup, 2026-11-17

The owner supplies the [recovered input-union contract](./tick_input_union_and_feedback_direction.md).
The previous proposal for an explicit gain-transform framework is withdrawn.
Its rationale assumed the regressed full-only replay model; it is not a reason
to preserve that regression. Earlier reasoning is retained in the
[pre-recovery snapshot](./historical/tick_coverage_before_intent_recovery_2026-11-17/output_gain_implementation.md).

Restore acyclic input-union coverage and the feedback-SCC-local full override
first. Resume gain with ordinary/custom multiplication nodes and the existing
control, replay, specialization and migration mechanisms. Remove unused or
harmful unstaged work from the rejected approach; retain reusable semantic
control identity, persistence, UI and snapshot requirements. Builds, tests and
profiling remain owner-run; staging and commits remain owner-managed.

## Source checkpoint after recovery, 2026-11-17 (unverified)

Restoration now includes `TickReplayMode::input_union`, generated union
propagation, explicit semantic-feedback-SCC replay exclusion, and a full-live
coverage override for feedback members. A compile-time proof can also recognize
acyclic unions fed by full realtime sources; it avoids unnecessary background
coverage metadata for ordinary gains. It does not gate feedback validity.
Sparse realtime recipients retain coverage metadata so their own input fallback
still applies. New/updated regressions exercise union versus full replay,
all-input-union feedback and sparse downstream delivery.

The gain-specific LLVM emitter, specialization flags, cache-key branch and
native Tick callback have been removed. Sequential scaling now constructs an
ordinary package `OutputGainNode`, with named static inputs, input-union replay
and the existing manual control on its open scale input. Registered concrete
channel/layout variants use ordinary package bitcode and LLVM specialization.
The removed `float32_lanes` capability enum is unnecessary; the type-owned
scaling operation remains for Sample/FFTBlock background views.

The remaining background scaling callback has a concrete purpose: Tock outputs
retain Region multiplicity and complete-buffer capability. For buffered Tick
outputs, a historical Tock view scales the original retained recording while
the ordinary Tick multiplier serves live execution. Both use the same frozen
prepared gain snapshot; random-access reads never acquire an extra live-stream
latency or overwrite the recording. This is separate execution of ordinary
multiplication in the two domains, not a new output-transform framework.

Unity still omits both stages. Control identity, persistence, gestures, UI and
snapshot activation remain necessary. The source is not yet verified: no build,
test, typecheck or profile has been run. Finish reviewing typed authoring,
coverage routing and migration, then ask the owner for verification. The new
buffered reconstruction/cache-priority decision is documented follow-up work,
not claimed implemented here.


## Owner verification handoff, 2026-11-17

The source pass now includes distinct live and historical routes for buffered
outputs. Historical reads retain the source's Region descriptors and buffered
complete-buffer capability; Sample/FFTBlock scaling writes derived contents.
Sequential and random-access stages have distinct migration identities.
Tiled member controls reach aggregate consumers without scaling other members;
aggregate controls retain their multichannel identity.

Preparation requirements are propagated upstream in one dependency pass across
sample/event connections, module boundaries and tiled aggregates. A gain that
can affect background work stays frozen until a prepared successor is ready.
This replaces a separate graph traversal for every output control and covers
routes that name a tiled aggregate instead of its concrete member.

Authored coverage callbacks retain their contract. Generated input-union and
full callbacks publish their own coverage; publication no longer overwrites an
authored replay callback's result. Added regressions cover live downstream
manual fallback after an input-union processor, recorded-byte preservation,
prepared gain replacement, sparse/overlapping Regions, FFT values, tiled
routing, persistence, RPC identity and frontend gestures.

No compiler errors are intentionally left as scaffolding. This remains
unverified source: the owner must build, run C++/TypeScript tests and typecheck
the Vue client. Only fast source checks were performed: whitespace checks pass,
and the shared package fixture's largest string fragment remains below its
60 KiB limit. No builds, tests, typechecks or profiling were run by the agent;
the Git index was not changed.

The separate reconstruction/cache policy is recorded in
[the output capability contract](./output_capabilities_and_background_demand.md)
and [the shared storage implementation note](./shared_buffered_storage_implementation.md).
Its eviction/reconstruction implementation is still pending and is not part of
this gain pass.

## First full verification feedback, 2026-11-17

The owner reports 1,125 passing tests and five failures out of 1,130.
The buffered gain unit fixture omitted the interleaved package variant required
by recording outputs. The output-control RPC fixture omitted the required
address `path`. The replay test incorrectly expected its explicitly dense
`ReplayWindow` output to omit zeros in the sparse input's gaps. These fixture
defects are corrected, including the designated-initializer ordering warning.

Both background gain observation failures exposed a storage-selection defect:
the observation selected the private gained port, but its projection could
still choose a storage slot for the public raw source. Projection now resolves
storage against the selected private port. The recording regression also checks
the original recorded values before applying gain. These corrections need an
owner rebuild and test run. The agent did not run builds or tests.

The owner further clarifies cache admission and retention priorities.
Computed pages favor reclamation unless expected reuse justifies retention.
Buffered Region/Coverage pages favor retention within resource budgets.
Sequential/buffered recordings cannot lose their only recoverable contents.
This policy is recorded in the output-capability contract. Its implementation
remains follow-up work.

## Recording-identity regression correction, 2026-11-17

The owner reports 1,129 passing tests out of 1,130. The remaining recording
gain test used an anonymous node. `persisted_output_id()` assigns anonymous
outputs generation-local identities, so their recording does not survive an
edit as the test expected. The fixture now uses `with_node_identities()` and
asserts that the original recording retains its stable output identity after
the gain change. Recording-byte and scaled-value assertions remain in place.
This corrects the earlier attribution of both observation failures to storage
selection: the Tock observation needed that fix, but the recording test also
needed a retained node identity. The owner must rebuild and rerun this test.
No build or test was run by the agent.

## Recording gain index preservation, 2026-11-17

The next owner run retains one failing test. The recording now survives the
gain edit. Scaled values are one sample late: 7, 9, 11 and 13 instead of
9, 11, 13 and 15. The fixture's raw output declares one sample of latency.
Ordinary connection planning applied that delay to the private Tock gain input,
although this view must rescale retained values at their recorded indices.

`reads_output_gain_source_indices()` now identifies that compiler-owned Random
Access input through the existing `output_gain_source` annotation. Its read
latency is zero during connection inventory, projection normalization and
latency planning. The authored source latency remains unchanged. Ordinary Tick
gain inputs and other authored connections retain their existing delay rules.
The regression asserts the source latency and the view's zero read latency,
then checks scaled values and the unchanged recording after removing gain.
The owner must rebuild and rerun verification. No builds or tests were run by
the agent.

## Application gesture correction, 2026-11-17

The owner reports all 1,130 tests pass. Application testing then finds that
dragging an output knob has no effect and stops subsequent input gestures.
The owner's backtrace in `build-release/output-knob-freeze.bt.txt` shows an
idle server, including the command and compiler workers. It does not show a
server deadlock.

`manualControls.hover` still required `inputName` and rejected output hover.
`SocketRpcServer::Impl::process_command()` then replied with ID zero when
payload parsing threw before returning the request. The client awaited the
actual ID, so its ordered gesture queue could not advance. Hover parsing now
accepts one input or output name. Error replies recover a valid numeric ID
from the request envelope. Requests without a recoverable ID receive null.
New parser and socket regressions cover output hover and an invalid control
payload followed by a successful input gesture.

Output knobs in lane headers no longer display adjacent numeric labels.
Popover values and accessible knob values remain available. Scrubbing now
computes travel from an anchor at the current sensitivity, rather than adding
rounded value increments. Sensitivity changes and hard bounds re-anchor the
gesture. Default-plateau progress remains physical travel. A regression checks
that one movement and many small movements both reach exact zero from unity,
while nearby nonzero values remain reachable.

The startup log now includes the spawned server PID. No builds or tests were
run by the agent. The owner must rebuild the server, test and reinstall the
client, then restart the running server/client session to clear the request
that is already waiting for the old unmatched error response.


## Nonnegative magnitude and polarity toggle, 2026-11-17

The owner confirms the application gesture correction works and requests a
nonnegative output knob with a separate invert toggle. This pass changes the
client only. Magnitude is the absolute signed gain and has minimum zero.
Gestures, numeric entry and default reset preserve polarity. The invert button
keeps magnitude and commits the opposite signed gain through the existing
interaction transport and history. Input knobs retain their existing semantics.

At zero, the editor retains inversion as VS Code webview state. It sends no
backend mutation for a polarity change that leaves gain zero. A later magnitude
increase uses the retained polarity. Nonzero server values, including undo,
redo and observed remote values, determine polarity when displayed.

Regressions cover inverted scrubbing through zero, reopening with an inverted
zero preference, one-gesture inversion, nonnegative numeric entry, reset and
input/output independence. The owner must run `npm test` and
`npm run typecheck`, then rebuild/reinstall the extension. The agent did not
run builds, tests or typechecks. Backend source and protocol remain unchanged
by this UI pass.

## Predictive hover after storage-changing settlement, 2026-11-17

The owner reports that an output-gain edit prevents later input hover from
preparing a dynamic variant until a click starts an interaction. Settled
code accepted through realtime replacement clears `settling_controls_`.
`ProjectGraph::compile_graph()` did not clear it when the same settlement
required a structural storage transition. The next hover updated its address
but skipped predictive scheduling because settlement still appeared active.

After executor acceptance, structural compilation now clears that flag when
the accepted configured graph matches the current settled controls. It then
prepares the latest hover using the existing predictive path. Failed or stale
compilation does not release the flag. Preparation still does not activate
hover-only code.

Two scheduling regressions exercise a real output-control identity with a
storage-changing mock executable. One hovers an input after output settlement.
The other changes hover while the settling transition is blocked, then checks
that completion prepares the latest input without activating it. The owner
must rebuild and run C++ tests. No builds or tests were run by the agent.

## Open design question about overlapping symbolic outputs, 2026-11-23

`ProjectSampleConnection::outputs` retains the selected symbolic address.
`apply_sample_connection()` resolves those addresses and deduplicates their
concrete channels. `ConfiguredSampleConnectionSource` retains the channel and
detach information. At that point GraphJit cannot distinguish a connection
made through symbolic output A from one made through symbolic output B when
they resolve to the same channel. The current output-gain precedence therefore
selects a single control for both consumers. A live gesture can temporarily
change which control wins.

Combining settled factors before fan-out would apply both gains to every
consumer. A branch between two gain stages needs the upstream signal at that
branch. If A and B expose separately adjustable outputs, the proposed design
first preserves the selected output through connection resolution, then lowers
each routed signal with its own gain. This includes deciding how explicit
selections through both aliases contribute to the same input, and how
source-authored connections identify the output. After that, lowering can
share transformed signals among consumers of the same output and consider
folding consecutive settled gains on a route with no intermediate consumers.
Tick interpolation and prepared Tock reads must agree on the selected route.

## Recording serial output routes, 2026-11-23

The initial analysis below is superseded by the
[named occurrence decision](#named-node-ref-occurrences-2026-11-23).

The owner accepted the [route and folding requirements](./graph_jit_direction.md#output-gain-specialization-owner-requirement-2026-11-16).
The remaining authoring question is how to specify that one symbolic output
forwards another. `GraphBuilderSymbolicNodes::attach_bundle_member()` maps each
`SymbolicNodeRecord` directly to concrete bundle ports. `NodeRef::operator[]`
passes the bundle and output index to `iv_builder_sample_port_from_output()`.
`GraphBuilderState::record_configured_sample_connection()` receives the resulting
channels and detach descriptors. Preserving the source selection in
`ProjectSampleConnection::outputs` distinguishes project connections through A
and B. It supplies no upstream link establishing that B forwards A and no
selected output for an existing C++ connection made through `NodeRef`.

The recommended approach is explicit output forwarding in the builder. It
would give a forwarded output an upstream output identity and let the same
concrete port support parallel exposures. Check whether the named-output
boundary created by `GraphBuilderPublicPorts::define_sample_outputs()` can
represent that forwarding before adding a new authoring operation.
Ordinary `NodeRef` aliases can
continue to name bundles without modifying the signal. An alternative is to
give named aliases forwarding semantics. That would require preserving alias
lineage through reference copies and projections, and adding a source name
could affect an authored signal route.

Settle the authoring rule before implementing output identity through the
builder, imports and project connection resolution. Then GraphJit can connect
routes to their gain controls, share transformed signals across their consumers,
and fold unobserved settled stages. The compiler should keep individual live
interpolation state and select coherent prepared values for Tick and Tock.

## Named node ref occurrences, 2026-11-23

The owner chose ordinary copy syntax and a DAG over the occurrences of named
`NodeRef` values. The builder records each occurrence with an optional upstream
index on the same `NodeBundle`. `NodeRef` copies retain that index. Declaration
annotations advance it and use annotations continue to record source spans.
The graph keeps repeated occurrences of a symbolic declaration separate and
retains them through scope snapshots, frozen invocation copies, imports and
the binary archive. Gain execution still uses concrete ports.

The source plugin emits annotations after the enclosing statement. In a
multi-declarator statement such as `auto a = make_node(), b = a;`, `b` may
copy `a` before `a` has an occurrence index. Fix annotation timing or preserve
copy provenance before annotation before using that form for gain routing.
Earlier source spans and bundle memberships cannot reconstruct that choice.

`ProjectNodePortMatcher` can select a declaration and concrete member. A
repeated declaration on one member can now have several forwarding occurrences,
and the project selector needs a rule for selecting or exposing them. The next
step is to retain selected occurrences through `SamplePortRef` and configured
sample connections, including the project-connection selection rule. GraphJit
gain composition and folding depend on those selected sources.
