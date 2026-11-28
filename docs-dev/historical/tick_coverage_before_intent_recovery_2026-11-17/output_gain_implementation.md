> Historical snapshot of `docs-dev/output_gain_implementation.md`, preserved 2026-11-17 before correcting the November 16 loss of input-union semantics. Non-normative. Replacement: [current document](../../output_gain_implementation.md) and [recovered contract](../../tick_input_union_and_feedback_direction.md).

# Output gain implementation

Status: active feature, 2026-11-17. The owner reports all 1,108 tests pass for
shared buffered storage and requests the next accepted feature: output gain.
Builds, tests, typechecks and profiling remain owner-run. Do not manipulate Git
staging or commits.

## Accepted contract

The owner clarifies that gain is a non-destructive `output * scale` transform.
It applies after the public output, including buffered outputs and previously
stored contents. Changing gain does not mutate node state or retained source
bytes. It changes the value consumers read, including background/random-access
consumers. Output shape, logical Region identity, sparse coverage and source
materialization validity remain unchanged. Consumer reads must use a coherent
selected graph/control snapshot.

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
