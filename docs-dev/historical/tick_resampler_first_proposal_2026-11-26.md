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

The replay contract in [Coverage and Background Evaluation](../coverage_and_background_evaluation.md)
originally excluded nonzero port history, output latency, and internal latency.
The replay summary in [Sequential Port Storage Planning](../sequential_port_storage_planning.md)
also treated any of these dependencies as disqualifying. The new replay contract
allows bounded output finalization latency when the node needs no earlier state.
The first implementation of that contract still required project-rate `Sample`
ports. It also excluded Region and Coverage inputs. The
[current replay path](../implementation_fronts_direction.md#tick-resampling-and-computed-source-demand)
carries exact coordinates and binds complete source buffers.

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
