# First Tick resampler proposal

_Historical record, 2026-11-26._

The first [complete-buffer resampler](../random_access_port_data_direction.md#complete-buffer-tick-resampling)
anchored every port and Transport Position to the project sample rate.
The second version let `position` and `out` resolve together. It still anchored
`source` to project rate.

An intermediate proposal added a second constructor argument,
`source_pace_exponent`, to set the source pace relative to `position` and
`out`. Zero meant equal rates, `1` meant twice the output rate, and `-1`
meant half. The [current resampler](../random_access_port_data_direction.md#live-resampling-from-computed-sources)
gets the source grid from its connected producers.

The first version of inferred source pacing used `pace().independent()`
in `constrain_ports`. [The current port pace rules](../graph_jit_direction.md)
use assignment syntax.

The replay contract in [Coverage and Background Evaluation](../coverage_and_background_evaluation.md)
originally excluded nonzero port history, output latency, and internal latency.
The replay summary in [Sequential Port Storage Planning](../sequential_port_storage_planning.md)
also treated any of these dependencies as disqualifying. The new replay contract
allows bounded output finalization latency when the node needs no earlier state.
The first implementation of that contract still required project-rate `Sample`
ports. It also excluded Region and Coverage inputs. The
[current replay path](../implementation_fronts_direction.md#tick-resampling-and-computed-source-demand)
carries exact coordinates and binds complete source buffers.

Immediately before input history support, [Coverage and Background Evaluation](../coverage_and_background_evaluation.md#13-authored-port-schema-retention-and-replayability)
required zero history, zero internal latency, and no `State` or `TockState`.
[GraphJit Direction](../graph_jit_direction.md) still described an earlier
strictly pointwise contract. It excluded history, latency, Random Access input,
and authored coverage propagation. It directed position-addressed work to Tock.
Bounded input history now lets replay recover input values independently for
each demand. The output history restriction still applies.

The [live computed-source design](../random_access_port_data_direction.md#live-resampling-from-computed-sources)
previously took a `preparation_samples` argument on both builtin resamplers.
Zero required complete buffers and added zero latency. A positive count
allowed computed or buffered sources, had to be at least the compiled root
block size, and required project-rate `position` and `out`. The node used that
count for the source preparation lead, output finalization latency, and
Sequential position history. The current graph planner chooses the lead from
the connected producers and compiled block size.

The first [deferred request proposal](../implementation_fronts_direction.md#tick-resampling-and-computed-source-demand)
suggested recording an output execution range and transport selection per
request, and letting the node retain positions in Sequential history or a
compact handle in `State`. It called for discarding superseded work on seek
and reset, and left the treatment of requests whose history carries through a
graph cutover open. The replacement specifies how the execution deadline
identifies results and which work a seek supersedes.

The first deferred implementation connected the bounded request queue to the
background transaction. `TickInvocationWorkspace` could not submit to it.
The worker could prepare a selected prefix, but the foreground had no way to
adopt that prefix or retain its data through later callbacks. The
[live Tick implementation](../implementation_fronts_direction.md#tick-resampling-and-computed-source-demand)
connects these operations and retains outstanding demands in the worker.

One proposed resampler design kept the zero-history rule by retaining input
frames in the executor and invoking Tick when the requested taps were ready.
The [accepted design](../implementation_fronts_direction.md#tick-resampling-and-computed-source-demand)
uses bounded Sequential input history to hold the original position.

Before the output history and latency split, `ContinuousOutputPort::get()`
accepted offsets through `history + latency`, inclusive. An output with
latency alone could read pending values and use them as recursive state.
[GraphJit Direction](../graph_jit_direction.md#port-history-and-latency-are-node-owned-semantic-state)
now defines separate permission to read history and to revise pending values.

## Replay discovery before the deadline pipeline

The following implementation notes, from [Implementation Fronts](../implementation_fronts_direction.md#tick-resampling-and-computed-source-demand),
describe the split resamplers and the discovery alternatives on 2026-11-26.
The current implementation runs consecutive Tick ranges and prepares due taps between calls.

The earlier alternative is to derive coordinates ahead of time from Tock or
replayable Tick and prepare their source taps before foreground execution.
It can eliminate the finalization lead when the coordinate stream really is
available off-thread. It cannot serve coordinates first produced from live
inputs. Proving that earlier readiness and implementing live request delivery
are separate dependencies. Both can eventually share sparse background demand
and prepared-window access.

`BackgroundEvaluationTransaction::prepare_evaluation()` seals input views before
the imported Tick callback runs. A replay callback that requests computed source
values would produce that demand after the views are already sealed. Selecting a
whole source buffer would defeat sparse demand. A separate resampler callback
would duplicate the coordinate and tap selection. A replay implementation needs
a discovery step before the final evaluation. It can reuse the transaction's
existing recursive discovery and selected data version. Live Tick already
delivers requests from coordinates that only exist during foreground playback.

Observed replay constraint. `DeferredLinearResamplerNode` stores pending taps
across live callbacks in `State`. The replay declaration requires a node without
`State` and currently accepts only complete buffers on Region and Coverage inputs.
The existing reverse replay pass requests full input coverage for those ports.
Computed outputs cannot supply a complete-buffer input. Sparse computed-source
replay therefore needs a way to collect tap requests before the final evaluation
frame binds the source views.

Proposed replay path. Run the stateless Tick callback over the selected replay
ranges with the resolved `position` values. In this pass, `request_input()`
collects source taps and the executor discards the callback outputs. Prepare
those taps from the same data version before the final replay call. This adds
one callback pass when the positions determine all tap requests. Allowing an
authored `propagate_reverse_coverage()` on replayable Tick nodes is an alternative
that adds a separate dependency callback. Settle the discovery contract before
changing replay eligibility or implementing early Tick execution.

The accepted resampler interface uses one non-template node and one Tick
callback for foreground and background execution. The node does not own
`State`. The history of a submitted request still needs an owner.
`DeferredTickInputRequests::Request` keeps the integer tap coordinates and
the deadline. It does not keep the original fractional `position` value.
The position supplied by a later Tick callback belongs to that callback,
even when it consumes taps that an earlier callback requested.

The accepted design keeps the original value in the `position` port's bounded
Sequential history. Background replay now expands demand to the left by each
input's history and binds those values before it enters Tick. It also extends
invalidation to the right when a changed value can affect later callbacks.
Output history still disqualifies a node from replay. The resampler still
needs historical position coverage and sparse tap discovery before it can
switch to this path.
[Earlier ownership alternatives](#first-tick-resampler-proposal)

`TickSampleContext::input_has_coverage()` tests the current value.
`block_extended()` provides earlier values, including fallback values for gaps.
An uncovered position and a valid position of zero can have the same value.
Reconstructing earlier coverage from the current project coordinate fails across
a seek. The position history must retain the coverage decided when Tick staged
each value. Then the resampler can pair the retained coordinate with its exact
source taps at the deadline.

The former [computed-source resampler](../random_access_port_data_direction.md#live-resampling-from-computed-sources)
kept the integer taps and their weights in node `State`. That callback ran live,
and the separate complete-buffer callback could replay. Both now use
one callback and executor-owned request records.

The earlier [Replay contract](../coverage_and_background_evaluation.md#13-authored-port-schema-retention-and-replayability)
allowed complete Region and Coverage input buffers and selected a whole buffer
version. [GraphJit Direction](../graph_jit_direction.md) required the background
transaction to install immutable Random Access views before replay invoked
Tick. The deadline pipeline preserves that rule for each invocation and
can prepare the contents of a windowed input between invocations.

## Request ownership before structural cutover support

The earlier [Tick resampling status](../implementation_fronts_direction.md#tick-resampling-and-computed-source-demand)
recorded that a new compiled execution generation always started a fresh
request queue. A compatible replacement of steady code kept the existing
queue. Transfer of pending requests and their selected source data was open.
The structural cutover now transfers the compatible requests at the audio
boundary and can borrow the predecessor's prepared input windows.

## Seek alternatives for early Tick

The earlier [early execution status](../implementation_fronts_direction.md#tick-resampling-and-computed-source-demand)
considered two seek policies. One kept a checkpoint or recomputed enough of
the old segment to restore the state at the seek cut. The other delayed the
seek until playback reached a cut that no early branch had crossed.
Both policies preserved played output. A prepared result could be reused
only if its dependencies were valid on the joined execution sequence.
The owner chose the bounded drain policy in the
[playback design](../project_playback_and_presentation_grids_direction.md#background-preparation-pending-seeks-and-missed-deadlines).

## Rejected early Tick opt-in

Preserved on 2026-11-26 from the
[implementation front](../implementation_fronts_direction.md#foreground-prefixes-for-early-tick)
and the [playback design](../project_playback_and_presentation_grids_direction.md#background-preparation-pending-seeks-and-missed-deadlines).
The planner prototype used `can_tick_ahead` in package ABI 18. Its default
followed the intrinsic replay declaration. The prototype kept device input
paths, buffered Tick writers, live manual controls, and SCCs reachable from
those sources through current Tick deliveries on the foreground. A committed
recording read or Tock delivery could feed an early SCC independently.
The playback design assigned feedback SCCs to Sequential foreground execution
and treated a possible pure, isolated Sequential renderer as separate from
ordinary pointwise replay. The owner rejected the opt-in and confirmed that
`is_live` propagates downstream. Feedback stays within the Sequential Tick
schedule and follows the same live dependency rule.
