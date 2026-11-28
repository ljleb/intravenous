# Background-fed input playback history, 2026-10-23

_Status: historical, non-normative. These records describe a representation of
background-fed Sequential inputs that was superseded the same day, and the
cross-kind transition machinery built on it. They were removed from
[`implementation_fronts_direction.md`](../implementation_fronts_direction.md) when
the node-owned timeline representation replaced them. The current status is in that
document's Front 4 and Front 6 sections._

## Why the representation was superseded

`TickInvocationWorkspace` rebuilt `[P - history, P + block)` of every slot from the
current snapshot by `GlobalIndex` on each invocation. That contradicts the normative
definition of Sequential history in [GraphJit direction](../graph_jit_direction.md):
"the preceding resolved `history()` in Tick invocation order", Sequentially adjacent
across a resume/seek seam, with freshly initialized neutral history before a node's
first invocation. The transition machinery recorded below was matched to that
rebuilt history, so it was removed together with it.

Two further defects in the superseded representation were found while replacing it:

- The workspace wrote `sequential_begin_ordinal` into each slot's own binding, but
  `TickInvocationCall` exposed copies made at construction with ordinal zero. The
  generated root therefore read slot frames `(0 + i) & mask`, while `populate` had
  written frames `sample_index.bits() & mask`.
- Mixed sample transition staging bound every channel of a multichannel ring at
  channel 0. This was corrected in source before the representation was removed.

## Front 4 status bullets removed

> - *Done in source:* Front 4 now exposes the predecessor playback window needed by
>   Front 6. `TickInvocationWorkspace::playback_slots()` exposes its fixed Sequential
>   sample/event bindings, and `TickInvocationWorkspace::playback_end()` identifies the
>   exclusive end of the last populated window. For a pure `invocation_supplied` input,
>   background-to-realtime transition transfer reads the final predecessor playback
>   binding. Realtime-to-background transition specialization appends a private sample
>   or event ring, binds the transition root to it, copies inherited realtime history at
>   cutover, stages each current `TickInvocationCall` window into it, and returns to the
>   ordinary invocation-supplied binding after expiry. Background delivery on both sides
>   needs no realtime transfer. Focused transition tests cover sample binding latency and
>   channel delay, event-window selection, both direction choices, and suffix storage
>   realization.
> - *Cross-front limitation found:* a mixed input marked
>   `combines_invocation_supplied` has only its background contribution in the
>   `TickInvocationWorkspace` slot. Its combined callback-facing sample scratch or event
>   sequence is classified as `callback_only`, so a graph revision that must migrate
>   that input's history fails explicitly. Front 4's mixed callback execution is
>   implemented; resolving this case requires Front 6 to retain or reconstruct the exact
>   combined input state rather than treating the background slot as that state.

> - *Verification gap:* the 592-test run did not exercise this path. No compiled
>   GraphJit fixture runs a Tick primitive whose Sequential input is supplied through
>   `TickInvocationCall` slots: the generated overlay
>   (`overlay_tick_sequential_bindings`), mixed combination
>   (`combine_tick_sequential_sample_bindings`,
>   `combine_tick_sequential_event_bindings`), and transition staging
>   (`stage_tick_sequential_sample_bindings`) are covered only by planning and helper
>   tests. "Verified" above therefore covers the planned shapes and runtime helpers,
>   not generated execution of background-fed inputs.

## Front 4 in-progress handoff note, replaced on completion

> **Reopened: node-owned history for background-fed inputs (2026-10-23; in progress,
> the working tree is not expected to compile until this note is replaced).**
> - *Feature:* a Tick primitive's Sequential input fed through a `TickInvocationCall`
>   slot keeps the history it consumed, in Tick invocation order, with neutral history
>   before its first invocation, as [GraphJit direction](./graph_jit_direction.md)
>   requires for every Sequential input. The Front 6 friction record below explains
>   the defect.
> - *Destination representation:*
>   - A pure background-fed input with nonzero history, and every mixed input, binds
>     `channels` (samples) or `storage` (events) to a node-owned retained timeline in
>     `NodeStorage`.
>     - A sample timeline is planned like a disconnected Tick output's retained
>       buffer (ring or compact carry, initialized to the input's neutral value) and
>       addressed by the generation pace cursor.
>     - An event timeline is a persistent ring with seek rebasing and pruning.
>   - Before the callback, the root writes only the current block into the timeline:
>     the slot window for a pure input, or the node's combination of the realtime
>     operand (`realtime_operand_channels`, `realtime_operand_storage`) with the slot
>     window for a mixed input.
>   - A pure input with zero history keeps the direct slot overlay.
>   - `TickInvocationWorkspace` slots hold only `[P, P + block)` at frames
>     `[0, block)`. This also removes a defect in which the generated root read the
>     construction-time ordinal (zero) of a slot that `populate` addressed by
>     `GlobalIndex`.
>   - Tick materialization demand covers playback positions only.
> - *Consequence for Front 6:* these inputs become ordinary `direct` retained state.
>   Graph revisions in both directions use the existing ring/compact-carry transfers
>   and transition suffixes. The playback-slot transfer source (`invocation_playback`,
>   `invocation_supplied`, `combined_invocation_supplied`, `RealtimePortStatePlaybackSlots`,
>   `TickInvocationWorkspace::playback_slots()`/`playback_end()`) and the Tick-to-Tock
>   staging transition root are removed.
> - *Layers:*
>   - runtime workspace and executor demand;
>   - sample storage planning;
>   - sample and event port planning;
>   - generated binding and staging emission;
>   - realization planning;
>   - transition planning and execution;
>   - tests;
>   - status documents.

## Front 6 sections removed

> **Front 6 follow-up exposed by Front 4 (2026-10-23):** the completed Front 6 cases did
> not include a Sequential input whose `PrimitiveSampleInputBindingPlan` or
> `PrimitiveEventInputBindingPlan` has `combines_invocation_supplied`. The
> `TickInvocationWorkspace` slot retains only that input's background contribution; the
> generated sample scratch or merged event sequence that the callback observes is
> `callback_only` after the root returns. Pure `invocation_supplied` delivery-kind
> changes now have a complete transition path, but migration involving the mixed form
> still fails explicitly. Front 4 is now verified, so Front 6 is reopened for this
> bounded case: either retain the exact combined input state or preserve enough source
> and combination metadata to reconstruct it. The background contribution alone is not
> the node-owned input history and must not be substituted for it.
>
> **Mixed-input transition implemented in source (2026-10-23; compiled without
> warnings, full suite passing, generated paths not yet exercised):** the follow-up keeps one representation of the callback-visible state on
> each side of the cutover.
>
> - *Predecessor reconstruction:* `SampleRealtimePortStateAccess` and
>   `EventRealtimePortStateAccess` gain `combined_invocation_supplied`. Its realization
>   keeps the ordinary realtime derivation in `retained_sources`/`materialization`/
>   `composition_sources` (samples) or `retained_storage` (events), and
>   `invocation_combination` names the `invocation_playback` slot. For samples it also
>   names the node's own `combine_sequential_input_values` operation
>   (`NodeImplementation::host_combine_sequential_input_values`). The steady callback
>   recombines its whole history-inclusive window on every invocation, so the cutover
>   transfer reconstructs it exactly. It evaluates the realtime derivation into the
>   target, then applies the same combination with the final playback window, in the
>   same operand order. Events merge both operands with their semantic source indices
>   into a suffix that has a source-index sidecar, reproducing the steady merge's
>   equal-time order.
> - *Successor staging:* a mixed successor always takes `transition_only_storage`. Its
>   binding keeps the realtime operand (`channels`, or event `storage`) and the
>   combined scratch or `combined_storage`. `staging_channels` (samples) and
>   `staging_storage` (events) name the private ring. The transition root performs the
>   ordinary combination or merge, then copies only the current block or window of
>   that completed result into the ring and binds the callback to the ring. The suffix
>   therefore never mirrors the realtime operand alone, so background history before
>   the cutover is not duplicated, and the ring is a complete `direct` source if
>   another revision arrives before expiry.
> - *Fresh channels of a staged sample port:* stable port-state identity is per
>   channel, so a revision that widens a staged input's channel layout leaves the new
>   channel without inherited state. That channel is bound to the same ring and
>   Sequential cursor (`restaged_window_channels`), and the transition root restages
>   its whole history-inclusive window from the callback-visible source on every
>   invocation. This presents the steady successor's fresh view of that channel.
>   Previously a pure staged port failed binding emission in this case, and a mixed
>   one would have reached an `IV_ASSERT` during lowering. *Correction (same day):*
>   copying the steady root's view repeats the Front 4 defect recorded below. Under
>   the normative rule a channel without inherited state has freshly initialized
>   neutral history, so this restaging is the wrong behaviour.
> - *Related corrections:*
>   - `build_steady_storage_transfers` seeds a mixed successor's future provenance
>     into its realtime operand by checking the operand's access.
>   - Transition realizations reset `invocation_combination` (and composition
>     sources), so a revision during the transition reads the suffix as `direct`.
>   - The retained suffix view does not advertise the cutover-only source-index
>     sidecar.
>   - The steady mixed merge order defect recorded under Front 4 is corrected.
>   - Mixed sample staging bound every channel to the transition ring's base address
>     with a one-value stride, so a multichannel ring received every channel at
>     channel 0. It now uses the per-channel offset and layout stride that
>     `sample_storage_binding()` assigns to every representation. The generated code
>     had never been compiled, so this was found by reading it, not by a test.
> - *Tests added:* the mixed merge helper with history on both sides and an
>   equal-time tie; planning of a mixed successor with future provenance; and cutover
>   reconstruction of a mixed event predecessor.
> - *Remaining for completion:* a compiled GraphJit fixture that runs a background-fed
>   Tick input through real `TickInvocationCall` slots in steady, mixed and
>   transition roots, for both cross-kind directions and the mixed case. The same
>   gap is recorded under Front 4.
> - *Implementation friction (unresolved; 2026-10-23):* this was first recorded here
>   as an open semantic question. The normative documents already answer it.
>   [GraphJit direction](./graph_jit_direction.md) defines a Sequential input's
>   history as "the preceding resolved `history()` in Tick invocation order", which
>   stays Sequentially adjacent across a resume/seek seam. Pre-start history is
>   freshly initialized neutral. [Coverage And Background
>   Evaluation](./coverage_and_background_evaluation.md) states that resume "does not
>   reset live Sequential history".
>   - *Observed constraint:* Front 4's `TickInvocationWorkspace` playback
>     (`SequentialSampleSlot::populate`, `SequentialEventSlot::populate`) clears and
>     refills `[P - history, P + block)` by `GlobalIndex` from the current snapshot on
>     every invocation. A background-fed input's history is therefore whatever the
>     snapshot holds at those global positions, not what the node consumed. This
>     diverges in three cases:
>     - after a seek, the history is the post-seek global neighbourhood;
>     - republished data rewrites history the node already consumed;
>     - on a node's first invocation, the history is real data instead of neutral.
>
>     Mixed inputs recombine that rebuilt history on every call, so they inherit the
>     defect.
>   - *Consequence for this front:* both the transition ring that keeps staged blocks
>     and the cutover reconstruction were matched to the defective steady behaviour.
>     The correct representation for a background-fed Sequential input is a
>     Sequential-cursor ring, as for realtime inputs. Each invocation writes only its
>     current block from the snapshot, and history stays as consumed. That ring is
>     itself node-owned history. A cutover in either direction then transfers between
>     ordinary retained rings, which may remove the Tick-to-Tock staging root added
>     for this front.
>   - *Status:* reported to the human co-author with this recommendation. No source
>     change has been made for it.

## Coverage And Background Evaluation status text replaced

[`coverage_and_background_evaluation.md`](../coverage_and_background_evaluation.md)
described the superseded slot sizing in its implementation-status list:

> publication rather than repaired inside a callback. Sequential sample slots own
> preallocated power-of-two rings
> sized from the specialization's maximum block size plus authored history. The
> callback fills only the bounded requested window from coherent materialization
> or direct persisted-region data and leaves absent frames at that logical input's neutral
> value, without allocation. Sequential event slots likewise own preallocated
> power-of-two sequences sized from the selected storage's aggregate event rate
> and maximum block size. Callback binding resets the count and copies only events
> in the requested absolute-time window; missing data is the empty sequence.
