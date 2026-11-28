# Early Tick latency-hiding proposal

_Historical implementation notes from November 26, 2026. Originally in [Implementation fronts](../implementation_fronts_direction.md). The owner clarified that early execution must preserve foreground semantics._

[Current implementation direction](../implementation_fronts_direction.md#early-tick-preserves-foreground-semantics)

The startup prefix uses the existing declared node latency. Removing
computed-source latency requires a separate early execution cursor and
enough additional prepared frames to finalize the first audible window.
The current worker prepares one prefix at a time.

_Implementation friction, 2026-11-26._

For a 64-sample computed-source lead, the first early callback submits taps
for project samples 0 through 63. The callback for samples 64 through 127
uses those taps to finalize the first range. The device must remain parked
until that range is final. The original `GraphExecutor::reserve_early_tick_prefix()`
opened one batch in `DeferredTickInputRequests`, and
`GraphExecutor::execute_tick_block()` published it only after the final
device slice. Waiting for the taps before rendering that slice would leave
the worker unable to see the request that the device is waiting for.

The existing discard path assumes a provisional prefix has never published
a request. Making those requests visible earlier requires keeping them
with the provisional playback work so that a paused seek can discard them.
Recording publication and the playheads still depend on consumption.
The original split root shared a Sequential cursor between the early and live
groups at each pace. Invoking the early root twice with that cursor would reuse
the same storage indices. Advancing the shared cursor twice would give
the live root the wrong indices. Changing the callback start alone also
leaves the compiled input delays unchanged.
[Deferred request publication](../../src/intravenous/execution/graph_executor.cpp#L3056) ·
[Startup discard invariant](../../src/intravenous/execution/deferred_tick_input_requests.h#L232) ·
[Sequential cursor commit](../../src/intravenous/graph_jit/realtime_root.cpp#L2487)

The general implementation needs a bounded range of prepared output between
the early and live groups. The worker must resolve requests before the early
callbacks that consume them. The graph plan must align the finalized early
outputs with live inputs at the handoff. A seek can join a new range after
the committed early range, with consumption deciding when the new position
becomes visible. A graph cutover must account for the same outstanding range
and its state. Keep the existing node API and the authored latency used by
callbacks. The prepared delivery range is a runtime responsibility.

A narrower alternative uses existing background replay to prepare the
outputs of deterministic, stateless branches. That can settle the first
computed-source regression for a replayable position stream. It leaves
stateful early branches on the current path. The general path needs the
execution and request ownership changes before the latency can be hidden
for those branches.

_Remaining cutover friction, 2026-11-26._

`GraphExecutor::activate_pending_at()` currently migrates `NodeStorage`,
switches `realtime_active_`, and closes the predecessor's background inputs
at the same moment. The existing `ForegroundPrefix` works with that model
because it delivers one reservation before the next early callback starts.
Once the early branch runs far enough ahead to hide its latency, the two
roots can reach the shared execution index at different device times. For
example, a cutover at execution index 64 can require the early successor
callback at 64 to finalize output for the predecessor's foreground range
starting at 0. Waiting for the device to reach 64 before activating either
root stalls that range. Running the callback at 64 with the predecessor
would execute it under the previous wiring.

Keep the shared execution index. Split activation by root so each root
migrates its state when it reaches that index. Lower the crossing Sample
and Event dependencies to bounded storage for finalized delivery ranges.
The ranges need to survive activation of the early successor and remain
readable by the foreground predecessor. Each `TickTransferBuffer` stores
one invocation's transient allocations, while persistent port histories
remain in its `NodeStorage`. Holding only a reference to that buffer does
not retain those histories. The Sample bindings and
`RegionSpecializationSampleAddress` encode the original ring capacity and
index mask. Copying a short window and substituting its address would
still index the copy as the full ring. The lowering must give the
foreground a binding to a delivery representation sized for its read window.
Keep early input request publication and
retirement with the early callback, and commit recordings and transport
progress in device order. This is the remaining dependency for first-frame
playback and reset with computed inputs. The late root still reads with the
current declared latency. It needs a binding to the finalized range from
the early root, including when the callbacks that finalize it execute
after a cutover.

A sealed `TickRecordingBatch` holds a producer chain outside its
`PendingQueue`. The background drain can see that chain only after
publication. A staggered cutover must keep the predecessor's output queues
open until the foreground has rendered or discarded every range that can
publish into them. An empty queue alone does not establish this boundary.

The owner approved allowing a brief playback delay when a graph edit
introduces a live dependency needed to finalize compatible pending output.
The output order and shared execution cutover still apply.
[Playback contract](../project_playback_and_presentation_grids_direction.md#background-preparation-pending-seeks-and-missed-deadlines)

_Causality at an edit that reverses a Tick dependency, 2026-11-26._

The preceding cutover plan assumes that an early sample becomes final if
the scheduler waits long enough for its new live inputs. Two individually
acyclic graphs can violate that assumption:

- `RevisableCutoverSample` writes a Sample every callback and can replace
  the preceding sample through its one-sample output latency.
- `LiveInvertingSampleDelay` emits its saved Sample, then saves 1 for an
  input of 0 and 0 for any other input. Its `State` survives a graph edit.
- Before the edit, the live node reads the revisable node. At the shared
  execution index 64, remove that connection and connect the live output
  to the revisable input. Keep the revisable output's configuration.

With its declared read delay, the live callback at index 63 reads the
finalized sample from index 62 and saves 1. At index 64 it emits 1, and
the new connection delivers that value to the revisable callback, which
uses it to finalize sample 63. The native fixture checks the ordinary
cutover's output and confirms that the revisable node moves to the live
part.

If playback delivers revisable sample 63 to the old live callback at
index 63, the live node must save the inverse of that sample. Its callback
at index 64 emits that saved value, and the revisable callback uses it
to finalize the same sample 63. The result must be both 0 and 1. A hardware
wait changes when execution happens and cannot resolve this dependency.
With a computed input, the same case can use the scheduler's preparation
lead as the output latency and combine a due result with a current live
input when updating the pending Sample. The current Tick API permits
that combination.

This affects how `resolve_tick_preparation_leads()` chooses the lead, how
`plan_latency_compensation()` turns source latency into read delay, and
which state `GraphExecutor::activate_pending_at()` must preserve at a
cutover. A separate delivery ring can preserve ready output across a
cutover. It cannot provide the missing input in this example.

One direction keeps the shared cutover and moves computed input lookup
into the worker's execution of an early callback. An early callback could
receive requested values before it completes and therefore use a zero
scheduler preparation lead. Live Tick would keep its advance requests,
and authored latency beyond preparation would keep its current meaning.
That requires a worker-only lookup path and consistent readiness handling
before the device consumes the range. Another direction keeps latency
hiding for revisable outputs and lets the old wiring finish selected
pending samples. That changes the shared-cutover contract and requires
choosing when old Tick state stops advancing and which state the successor
inherits.

Resolve the contract before changing playback read offsets or splitting a
generation's cutover. The ordinary cutover test uses
`ReversedTickDependencyUsesLiveStateToFinalizeAnEarlierSample`.
[Port window ownership](../sequential_port_storage_planning.md#port-windows-are-semantically-node-owned-even-when-storage-is-shared)
