# Project playback implementation

Status: active implementation, 2026-11-17. The owner confirms the page-cache
tests pass and has run its benchmark. Playback is the next authorized feature.
Builds, tests, typechecks and profiling remain owner-run; Git staging belongs
to the owner.

The normative contract is
[Project Playback And Presentation Grids](./project_playback_and_presentation_grids_direction.md).
Implement one coherent path: `GraphExecutor` applies ordered session commands
at Tick boundaries, tracks the background and foreground playheads, prepares
background windows, and selects actual covered background
inputs even while paused. It delivers selected Events once per applied paused seek.
Live Tick processing and history continue. RPC exposes
accepted commands and coherent applied snapshots. Vue draws both playheads,
displays samples and seconds, and provides controls independently of
viewport navigation. No graph rebuild or project undo entry follows a seek.

Implementation order:

1. Trace `GraphExecutor`, `SystemAudioDevices`, background preparation, session
   permissions/RPC ordering, and the timeline presentation. Record command and
   playback-window ownership before selecting the interfaces.
2. Implement ordered command application, coherent bounded snapshots, continued
   Tick execution during pause, held Sample selection and once-per-applied-paused-seek
   Event delivery, and preparation before applying resume/running seek. Preserve the
   single shared foreground index and its physical Sequential history at seeks.
3. Wire typed session operations, JSON-RPC, extension commands and timeline
   rendering. Keep viewport position independent; never extrapolate across a
   seek or pause. Do not add tempo or graph-authored grids to this feature.
4. Add regressions after the interfaces take shape, then hand verification to
   the owner. Report unexpected semantic or ownership friction before changing
   the accepted contract.

No playback source change is claimed by this initial plan. The optional
node-facing callback playback structure follows a concrete node requirement;
it is not required for ordinary Tock clips or the initial playhead.

## In-progress checkpoint

The memory feature passed the owner's full suite; its measurements are in
`page_cache_implementation.md`. Playback source is now being implemented.
No build, test, typecheck or profiler has been run by the agent.

`ProjectPlayback` now owns a fixed 256-command queue, one control-side command
order, audio-only applied state, and coherent atomic snapshots. Running is the
initial state, preserving the existing startup behavior. Paused project position
and advancing Tick coordinate are separate. `GraphExecutor::render_block()`
applies commands and generation activation before rendering. The existing
explicit-index `tick_block()` remains the deterministic graph-execution entry
for callers/tests; device rendering uses the session entry.

**Staged-source divergence (new intent, 2026-11-17):** `TickInvocationFrame`
currently selects neutral Sequential and empty Region/Coverage background inputs
while paused. The current `LiveTickCoverageState`/`TickMaterializationSnapshot`
paired metadata represents that superseded policy. These are implementation facts,
**not** accepted delivery semantics. Replace them with held covered Sample selection,
Event audition on applied paused seek, and normally pinned Random Access views.
Likewise, `ProjectPlayback::begin_block()` currently relocates `execution_index_`
for every seek; a paused seek must not relocate it. `GraphExecutor::render_block()`
currently advances playhead/execution after an unprepared `tick_block()` return;
an unsuccessful Tick block must not advance either. The capture path's single
latency-shifted contiguous global range must be replaced by seam-aware physical
finalization/address metadata. Do not add a pause-wide recording prohibition.
The accepted design is
[Project Playback And Presentation Grids](./project_playback_and_presentation_grids_direction.md).

RPC parsing/routes and the SocketRpcServer/GraphExecutor bridge were added for
`playback.get`, `playback.setRunning`, and `playback.seek`. Applied/accepted
command indices and segment identifiers use decimal strings. Read needs
`project.read`; controls need `project.write`, without a project revision,
autosave, or history mutation.

Remaining work: replace the staged pause suppression and seek/recording seams,
add preparation/readiness and requested-versus-applied UI behavior, finish the
client path and controls, regression tests, source review and owner verification.
The foreground all-or-nothing underrun contract and actual live-input stale-drop
policy need tests. The active UI is `GraphEditorProvider` /
`webview/graph/GraphEditor.vue`, not the deprecated LiveGraphViewProvider.
Use a separate playback host message to avoid rebuilding graph state on each
position update. Ruler click seeks, Space in the graph toggles playback outside
text inputs, and command-palette actions provide play/pause and numeric seek.
No extra permanent instructional text or toolbar buttons are needed. Playback
position must stay independent of the viewport transform.

No known intentional compiler errors, but the incomplete feature has not been
compiled. New files include `runtime/project_playback.h` and the
`socket_rpc_graph_executor_bridge.{h,cpp}` pair; the latter is registered in
the runtime CMake source list. The owner retains Git staging and existing
`projects/simple_sine` changes. Do not stop at this partial checkpoint.

## Implementation friction: recording before finalization

Status: paused for an implementation decision. This is an observation about
the current source, not a change to the playback contract.

The checkpoint calls for splitting the finalized range at a seek. That assumes
capture still knows which positions were `written`, `voided`, or `untouched`
before those positions became final. The current capture boundary loses that
information even without a seek.

`ReflectedContinuousOutputPortBinding` in `graph/reflected_node_operations.h`
gives `SampleCaptureSlot::capture()` a physical `sequential_begin_index` and
one `disposition` for the current invocation. It does not retain the earlier
invocation's `disposition`. In `runtime/tick_invocation_frame.cpp`, Sample
capture returns immediately on `untouched`. For example, if a recorder writes
`[100,104)` with latency two and leaves the next block `untouched`, capture
drops the still-eligible values at `102` and `103`. After a seek to `500`, it
also cannot recover their project addresses from the current callback index.

`ReflectedEventOutputPortBinding` also supplies only the current invocation's
`disposition`. `EventCaptureSlot::pending_events` retains occurrences, not the
range covered by an explicit empty write. If the next block is `untouched`, an
empty pending list causes capture to return without publishing that range.
`prune_event_ring()` in `graph_jit/realtime_root.cpp` also rebases retained
`TimedEvent::time` for live consumers across a seek. That time is no longer the
project address to publish in a recording.

`RealtimeProducedRecordHeader` in `runtime/realtime_produced_record.h` describes
one `begin` and `range_extent`. It can describe each final range separately,
but it cannot recover authoring decisions that were lost before publication.
`TickInvocationWorkspace` currently reserves queue blocks from the maximum
payload for one record per invocation and the maximum invocation count.
Publishing separate ranges adds headers and block boundaries. More than one
seek or disposition change can fall within a single finalization window.
The realtime reserve must account for that fragmentation before a producer runs.

One option is to keep separate recording histories in the capture slots. That
requires the generated callbacks to send each authored range before its
`disposition` is reset. If the slots also copy all still-revisable payloads,
they duplicate Sample ring storage and must reconcile later revisions.
Splitting the range in the background consumer costs less but cannot recover
the missing fields. Resetting or flushing history at a seek changes semantics.

Recommended: retain bounded address and disposition spans at the authored
callback boundary. Key each span to the physical Sequential positions that it
describes. Keep Sample values in their existing ring. Preserve Event recording
addresses separately from rebased live timestamps. Resolve the spans into
ordinary addressed `RealtimeProducedRecordHeader` records when values become
final. Plan the reserve for fragmented output and define how open spans follow
compatible output history during generation transfer.

Dependency change if accepted: implement authoring and finalization in the
generated Tick root, capture workspace, and producer reserve as one prerequisite.
Then finish `GraphExecutor` readiness and playback selection, followed by RPC,
the requested and applied cursors, and end-to-end regressions. Cover disarming,
explicit empty Event writes, and latency across several seeks before validating
the complete path. No source or tests changed in this review.

### Accepted address spans and queue capacity

The owner accepted the address and disposition spans. The next implementation
pass found a separate constraint in the queue. That pass assumed the existing
record format could carry the split ranges at a cost close to their byte size.

`RealtimeProducedRecordHeader::record_block_count` in
`runtime/realtime_produced_record.h` frames each record in whole 64 KiB blocks.
`BackgroundEvaluationTransaction::Impl::decode_produced_records()` in
`runtime/background_evaluation_transaction.cpp` expects a header at the start
of a block and consumes all remaining bytes as that record's payload. It
cannot decode two small ranges in the same block. `producer_capacity_policy()`
in `runtime/graph_executor.cpp` multiplies the worst callback's block count by
128 for the default high watermark. `AsyncCapacityManager::allocate_slab()`
allocates storage for all those blocks.

For example, a recorder with latency 512 can retain 512 values from separate
one-frame invocations at disjoint project addresses. The next 512-frame
invocation can finalize all of them at once. One block per fragment requires
at least 4 GiB of payload capacity at the default high watermark for that
producer alone. This is a calculation from the provisioned bounds, not a
measured allocation. The limit affects a generation even when playback never
seeks because the executor provisions the reserve before execution.

Smaller blocks would retain the current record format but add block traversal
to ordinary recordings. A lower watermark would reduce memory but shorten the
worker's margin to drain a burst. Neither change removes the one-block minimum
per fragment. The recommended alternative is to pack addressed ranges and
payloads into a callback's published block chain. Each range keeps its own
address and disposition, but record boundaries no longer imply block
boundaries. The reserve then accounts for payload bytes and headers together.

If accepted, change the producer writer, block reserve calculation, and
background decoder together. Keep each published callback chain complete so
the background transaction can still pin and retry a finite prefix. Then
finish the approved address spans and continue with playback. The trial
source changes were removed from the worktree before this decision. No build
or tests ran.

### Recording source checkpoint

The owner accepted packed callback chains. `RecordingSpans` now retains the
address and disposition of open Sequential positions. Sample capture reads the
finalized values from the existing ring. Event capture associates newly authored
occurrences with the open positions before live timestamps can be rebased.
`ProducedBlockWriter` packs records into a private block chain, and the
background decoder reads headers and payloads across block boundaries.
Capture reserves count acyclic primitive slices as well as SCC slices.
Regression source covers a seek across latency, Sample disarming, a written
empty range from a zero-capacity Event output, and packed overlapping records.
No builds or tests ran; Event capture capacity is still open below.

### Open friction: Event capacity before finalization

The capture reserve assumes `max_events_per_index` bounds the number of
Events that one finalization window can carry. `EventOutputPort::push_in_window()`
in `ports.h` instead admits Events up to the available space in the callback
sequence. A callback can place all of them at one legal timestamp. Several
short invocations can append to the same still-revisable position. Their
accepted writes can exceed both the size of `EventCaptureSlot::pending_events`
and the payload reserve for the slice that eventually finalizes that position.
Growing the packed block chain cannot recover Events that already overflowed
`pending_events`. The current failure signal is not a proof of correct sizing.

Provisioning the worst case from the callback sequence capacity and every
possible overlapping invocation preserves those writes, but can multiply the
retained payload and queue reserve by the output latency. Treating the declared
rate as a quota instead would change which writes `EventOutputPort` accepts.
Prefer planning Event authoring, retention, and capture around one explicit
storage bound. Determine whether the retained Event sequence already gives the
right admission boundary for recording, or whether capture needs to publish
addressed writes early for the background consumer to finalize. Decide that
before claiming the recording prerequisite complete. This affects Event storage
lowering, `EventCaptureSlot`, capacity and transfer planning, and the stress
tests for repeated short invocations and equal-time Events.

## Live coverage at two positions

Live Tick reads Tock Sequential coverage at the held playhead during pause.
Random Access and background replay continue to use requested project positions.
The preliminary exact-window approach and the invalid authored Tick coverage path
are [preserved for reference](./historical/tick_replay_and_playback_coverage_before_pointwise_correction_2026-11-17.md).

## Follow-up after the accepted fixes

### Event recording

`EventOutputPort::push_in_window()` admits writes against the callback's
Event storage. A latent output can accept those writes before GraphJit
merges them into its smaller retained delivery sequence. The merged
sequence therefore cannot bound all writes that recording must preserve.

GraphJit passes the callback buffer capacity `C` to `EventCaptureSlot`.
With latency `L`, only the last `L` executed positions can still be
revised at the beginning of an invocation. Each earlier invocation
executes at least one position, so at most `L` earlier invocations can
contribute pending Events. The current invocation adds at most `C`
Events. This bounds pending storage by `(L + 1) × C`. Across a root call
with at most `N` invocations of this output, at most `(L + N) × C` Events
can be published, plus the headers for all finalized address and
disposition spans. Compatible generation transfer carries pending Events
and provisions for the larger acceptance capacity before activation.

Capture reads the authored sequence before live coverage filtering or
retained-sequence merging. `RecordingSpans` retains addresses and explicit
empty writes independently of occurrence count. No per-timestamp quota
was added. The boundary regression fills five callback buffers at the
same timestamp and checks all 40 Events in authoring order.

### Live playback and readiness

`TickMaterializationSnapshot` provides the pinned position-addressed coverage.
`stage_live_output_coverage()` applies the generated `input_union` rule to
the inputs each Tick invocation actually receives. It retains one presence
flag per sparse output value in bounded `NodeStorage`. Full-live outputs do
not need this storage. A covered, empty held Event range contributes coverage
without delivering a new occurrence. A fallback value contributes no coverage.
Pause needs the held Tock data, not a new coverage table for each Tick window.

`GraphExecutor::render_block()` checks readiness and chooses the execution
index from the same root it uses to run Tick. A cutover migrates at that
index. The background actor tries exact demand without lookahead if
speculation fails and keeps preparing the applied window if the candidate
fails. Draining a closed predecessor does not require speculative playback
data for it. The device regression checks that a skipped Tick produces
device silence and that the next successful Tick reads the next physical
input slice, not the skipped slice. The editor shows a separate requested
cursor until the backend acknowledges its command sequence.

### Pointwise replay and delayed live reads

Static validation rejects Random Access inputs and authored propagation on
replayable Tick nodes. Background replay uses same-position coverage and
values from the selected snapshot. Live alignment delays do not apply to replay.

`live_projection_presence()` uses the physical Sequential index minus the read
latency when selecting sparse Tick coverage. The value and its coverage cross
seeks together. The seek regression exercises both covered-to-uncovered and
uncovered-to-covered reads in a graph with no contextual replay.

### Verification handoff

The staged and unstaged diffs passed `git diff --check`. Git staging is
unchanged. Builds, tests, client typechecks, and realtime cost checks still
need owner verification. Start with the recording boundary, held Sample
and Event coverage, Random Access, playback underrun and device-input
regressions, then run the playback client tests.

## Source review after the owner's test run

The owner reported 1,153 passing tests out of 1,156. The three failures
occurred during test setup. The device test sizes `tick_runtime.nodes`
and uses `finalize_background_runtime_plan()` for the background maps.
The equal-time Event test uses
a valid `ProducerCapacityPolicy` and exercises four separate recorded
addresses finalizing in a single callback. The RPC test dispatches from
the `SocketRpcServer` linker event source with the bridge bound, and checks
that command acceptance is separate from application. `app.cpp` also binds
that bridge before the server starts.

The playback underrun regression now covers a pending resume whose full
starting window is unavailable while the held position remains ready.
The timeline converts numeric ruler coordinates directly to sample indices.
Repeated running snapshots at the same position stop cursor extrapolation.
The next owner run passed the Event and RPC tests and exposed the missing
background maps in the device fixture. The device fixture with those maps
populated and the client changes await verification.

### Recovery when an accepted generation cannot prepare

`GraphExecutor::activate_pending_at()` can keep processing transport commands
and try another position against `pending_head_`. An accepted generation at
that head can still block all later generations if it cannot produce a
`RealtimePersistedState` at any position. A complete Random Access view that
cannot be prepared is one case where seeking to another position may leave
the same failure. `BackgroundExecutor::process_available_work()` retains
the generation for retry, while the next compiled cutover names it as its
predecessor. A later graph edit that removes the failing dependency therefore
cannot become active through that chain.

The [ordered cutover contract](./project_graph_application_architecture.md)
makes acceptance durable. Recovery would require an explicit decision about
when a preparation failure becomes terminal. One option is to keep the
failure fatal once the generation is accepted and report it to the session.
Recovery within the running session would require rejecting that cutover
and recompiling later work against the last active generation, with the
acceptance and migration rules updated together. Decide this before making
failed accepted generations replaceable.

### Terminal preparation recovery implementation

The owner confirmed the playback tests pass before this recovery work. The
owner also approved [rejecting terminally failed pending executables and rebuilding
their successors](./project_graph_application_architecture.md#recovery-from-terminal-preparation-failure).

`GraphExecutor` uses `ExecutionGeneration::activation` to arbitrate between
activation and private preparation failure. Immutable background plan errors
and `TerminalBackgroundPreparationError` can reject a pending executable.
Ordinary preparation errors remain eligible for another attempt. `ProjectGraph`
recompiles the pending suffix on its serialized compiler worker and retains
committed configurations and edits. The background owner cancels preparation
references and releases the rejected `ExecutionGeneration` objects. Full
definition snapshots live on pending generations for recompilation and are
released on the background path after activation.

Source regressions exercise a terminal error raised by a running preparation
callback, ordered recompilation after rejection, a new edit during recompilation,
and failures with no pending successor. Recovery builds and test runs remain
pending owner verification.

### Return to the selected start and ruler dragging

The owner confirmed that playback works in the editor and that recovery tests
pass. New sessions now start paused at zero. `ProjectPlayback` retains the last
selected start while running. An explicit seek sets that start in either
transport mode. A pause returns to the start when its held data are ready.
During preparation, `GraphExecutor::render_block()` can continue at the
applied position. Returning to the start preserves foreground Tick history
and does not audition the Event at that position. `TimeRuler.vue` captures
the pointer and sends a single seek for a click. A drag sends its first
position, coalesces moves to one per animation frame, and sends the final
position at release. The ruler, automation curves, and waveform views
display the pointer position during a drag. Live meters and controls stay clear.

`playback.drag` puts drag commands in the same queue as seek, play, and
pause commands. `ProjectPlayback` keeps the mode to restore and reserves a
queue slot for release. When the drag position needs preparation,
`GraphExecutor::render_block()` can hold the last applied position before
applying the request. The editor accepts Space with focus on the ruler or
blank graph surface, even when no lane is selected.

`ProjectPlaybackSnapshot::background_playhead` is the applied position that
selects background content. `ProjectPlaybackSnapshot::foreground_playhead`
is the next foreground Tick index. JSON-RPC carries them in `position` and
`executionIndex`, respectively. The foreground playhead is the faint cursor
on the ruler, automation curves, and waveform views. The ruler shows the
selected start as a triangle.

Regressions cover startup, running seeks, return to the start, drag command
ordering, drag release, and Event audition at a held position. Builds and
UI checks remain owner-run.

The owner also requested bounded delivery of crossed Events during a drag.
`TickPlaybackSelection` gives `SequentialEventSlot::populate()` a held
position and an audition flag. At that boundary the intermediate positions
and drag direction are already gone. `GraphExecutor` prepares one positional
demand for both Sample and Event inputs. Expanding that demand to the whole
drag span would materialize Sample data across it and could delay the applied
playback window, even when the span has few Events. The current drag uses the
existing Event delivery rules at applied positions.

To preserve crossed Events, retain the ordered drag segments through command
coalescing and give the background actor an Event-only demand. It can prepare
an ordered, bounded set of occurrences before the Tick boundary. The receiving
Sequential binding needs the source identities and a local delivery position.
The drag contract still needs a policy for direction changes and for segments
that exceed the Event budget. These decisions affect the playback command,
the background readiness record, and the bound on Event delivery at Tick.

### Deferred drag audition and MIDI clips

The owner prefers to drop crossed Events that exceed the drag audition
budget. The budget size and the behavior on direction changes remain open.

The proposed builtin MIDI clip is a module node that wraps a MIDI Event
primitive and an Event gate. The gate drops clip Events during paused Tick
execution, including paused seeks and ruler drags. A piano roll presentation
edits the stored Events using the automation curve as the editing model.
The module needs to release notes it started before a pause, because the
gate can block their later note-offs.

The proposal exposes the wrapped clip in the node picker and keeps its raw
primitive private to the package. `WorkspaceSession.graphDefinitions()`
currently lists the module and node type IDs that the server returns. If
private definitions must also be unavailable to direct creation requests,
apply that rule at instance creation as well as in the picker.

`GraphExecutor::render_block()` uses `ProjectPlayback::Block::state.running`
for the block it selects. One option is a runtime-owned source that publishes
that value on an existing Sample port. The gate can read it through an
ordinary connection using the current node API and port types. The
[optional Tick playback context](./project_playback_and_presentation_grids_direction.md#node-visible-playback-state)
is another option for supplying the same state.

### Playback preparation profile, 2026-11-18

A 45-second userspace profile of `simple_sine` recorded 14 background
underrun blocks. The background worker accounted for 198 of the 830 CPU
samples. Frequent sample locations included the allocator,
`IndexCoverage` updates, and `PersistedDataStore::Candidate::replace_sample_regions()`.
The capture leaves kernel wait time and most JIT symbol names unresolved.
It establishes where sampled CPU work occurs. It cannot assign each
missed deadline to a specific operation.

The two curves in the saved reproduction total about 12.7 MiB of raw mono
Sample values at 48 kHz. This excludes intermediate data and mappings.
`BackgroundEvaluationTransaction` retains unchanged playback windows and
requests missing values. `Candidate::replace_sample_regions()` receives the
replacement domain, the logical Region list, and newly produced windows.
It rebuilds the physical index from all retained windows.
`replace_tock_region()` matches mappings by
start and extent. Extending the trailing window can remap its unchanged
prefix. These publication costs can grow as playback fills the cache.

The proposed optimization has three steps.

1. Publish the applied or requested playback window as soon as its required
   data are ready. Track the ready interval and measure preparation time from
   request through publication. Use those measurements to tune the reserve.
2. Reuse mappings and index entries for unchanged cached windows. Preserve
   logical Regions, overlapping Regions, and existing reader leases when a
   replacement changes part of the payload.
3. Use the existing output cache to warm finite, inexpensive coverage in
   bounded batches after urgent playback preparation. Yield between batches
   for transport commands and edits. Admit this work within the cache's
   memory and preparation budgets. Cache validity follows the selected graph
   and input versions. Foreground snapshots lease the payload required for
   their prepared windows.

Compare cold seeks, repeated playback from warm cache, and playback during
edits. Record time to first ready block, underruns, newly evaluated frames,
mapping work, and retained bytes. No reserve or cache policy changed in this
investigation.

The owner reproduced the slowdown during a second 60-second capture. The
server logged 6,337 underrun blocks. The background thread spent 37.17
seconds in the kernel and 6.95 seconds in userspace. Kernel time rose from
8.63 seconds in the first 20 seconds to 15.00 seconds in the last 20 seconds.
The thread waited about 0.08 seconds while ready to run.

A four-second follow-up observed the process maps file open in 206 of 230
polls. The background thread read 2.17 GB through 545,571 read calls. Its
counters reported zero bytes read from storage. One process maps listing
was about 723 kB. The read volume was equivalent to about 750 full listings
per second. The read counters report totals for all files.

`BufferedPageAllocator::reserve_mapping()` scans `/proc/self/maps` for each
new mapping. `prepare_resident()` scans it again when the selection includes
newly locked pages. This evidence puts repeated mapping checks and mapping
reuse ahead of reserve tuning. Measure the number and cost of these scans
per transaction. Evaluate sharing the process-wide check across a batch,
with accounting for new mappings and page locks within the batch. Preserve
headroom for mappings outside the allocator and keep the existing failure
handling for kernel resource limits.

### Standalone playback reproduction

`iv_playback_benchmark` loads the checked-in project under
`benchmark/fixtures/playback` through the normal project and package loaders.
CMake copies the fixture into the build directory, where the package loader
writes generated artifacts. The fixture captures the two automation curves
from the November 18 live project and includes its own copy of the saw module.
It uses a benchmark package ID and sets the saw output gain to one.

The benchmark runs `GraphExecutor::render_block()` at the configured block
period. The existing stereo output node writes to a preallocated buffer.
Package and graph preparation finish before timing starts.

The owner observed that underruns build up during uninterrupted playback and
stop after a seek. The benchmark runs for 60 seconds, then seeks back to the
same start and runs for another 60 seconds. Both passes share the same
executor and cache. Use `--seek-sample` to select a different destination.

```sh
cmake --build build-release --target iv_playback_benchmark
build-release/benchmark/iv_playback_benchmark --trace-tsv /tmp/iv-playback.tsv
```

The benchmark reports the applied project position, underruns, and seek
latency. It reports missed periods in its own block driver separately.
The per-second rows and phase totals include the work to propagate coverage,
evaluate the graph, publish buffers, scan process mappings, and lock pages.
Timings include work in nested phases. For the mapping scans, `work` counts
mapping entries read. For sample index replacement, it counts indexed
windows. For mapping, page locking, and backing I/O, it counts bytes.
Use `--no-metrics` to compare the run with the timers disabled.

The package loader can rebuild stale package artifacts during startup.
The timed run measures playback and background preparation with the
in-memory output buffer.

### Standalone benchmark results

The owner ran two 60-second passes on the checked-in fixture. Both passes
completed 11,250 callbacks with zero underruns and zero missed driver periods.
The seek applied in 5.70 ms on the first pass and 5.62 ms on the second.
The standalone run reproduced the rising preparation cost seen in the live
profile. The live profile remains the reproduction for missed deadlines.

The mapping and residency checks made 12,366 scans of `/proc/self/maps`.
They read about 43.9 million entries and took 10.01 seconds in total.
This was 82.5% of the 12.12 seconds spent in background transactions.
The mapping and residency system calls together took about 0.97 seconds.
Graph evaluation took 0.17 seconds.

Replaying the same range after the seek reduced graph evaluation from
about 166 ms to 0.7 ms. The mapping and residency scans still took 6.61
seconds during that pass. `PersistedSampleReader` selects an intersection
of a cached window and the requested range. For a strict slice,
`PersistedSampleRegion::selected_view()` creates another mapping of the
page-backed data. A cache hit can avoid evaluation and still require
mapping and residency work.

Test a process-wide mapping check shared by each preparation transaction
before changing the reserve. Account for new mappings and page locks against
the sampled budget, and keep the existing limit checks and failure paths.
Then test passing clipped views of existing mappings to
`PersistedSampleReader`. Keep each view's visible range at the requested
intersection. Account for the backing pages that a shared mapping keeps
alive. Complete-buffer requests still require a contiguous view. Compare
both passes against the scan baseline before raising the playback reserve.

`BackgroundEvaluationTransaction::execute()` now shares a process mapping
budget across the mappings and page locks it prepares. The first operation
reads the process mapping count. Later operations charge their possible
growth against that estimate. Unmapping and unlocking can split an existing
mapping, so cleanup also charges possible growth. If the estimate cannot fit
the next request and the reserved headroom, the allocator reads a fresh count.
An operation outside a transaction reads a fresh count.

The benchmark assigns a scan to the operation that first needs it. Compare
the sum of `mapping_vma_check` and `residency_vma_check` with the baseline.
The allocator still checks its byte, run, and residency limits. The kernel
checks each mapping and lock request against the current process limits.

### Playback sample storage comparison

`iv_playback_benchmark --sample-preparation` selects a sample preparation
experiment on its executor. `remap` uses the existing production path.
`reuse` reuses an already mapped packed window for a strict cached subrange.
`pool` combines that cache read path with a resident 1 MiB pool of sample
slots for Tick. A Tick snapshot leases its slots through its existing payload
owners. A window that cannot fit in a slot keeps its ordinary residency path.
`arena` combines cached window reuse with stable addresses for contiguous
file runs in `BufferedPageAllocator`. One mapping of the page file supplies
those addresses. Fragmented windows use the ordinary mapping path, and each
window pins its own file pages. These modes leave the production default in place.

Run each mode on the same fixed project with the default two 60-second passes
and compare the per-pass traces. The first pass computes and caches curve
values. The second seeks back and reuses them. `--view-limit-kib` also lets
the benchmark measure the effect of smaller canonical windows. The benchmark
reports the upper bound of the 100 µs bucket containing the 99th percentile
transaction, output sample count and digest after each seek, and end-of-run
backing, mapping, and residency usage. `reuse_sample_window`,
`resident_pool_copy`, `resident_pool_fallback`, and `stable_sample_window`
show which operations each run actually exercised.

The sequential comparison on the fixed two-curve project produced the following
cumulative background transaction times. Each pass ran for 60 seconds at 48 kHz
with 256-frame blocks. All four modes rendered 11,250 blocks per pass with zero
underruns and zero missed driver periods. The audio sample count and digest
after each seek matched across modes. Seeks applied in 5.56–5.64 ms.

| Mode | Cold pass | Warm pass | Total | Transaction p99 upper bound |
| --- | ---: | ---: | ---: | ---: |
| `remap` | 3.49 s | 3.36 s | 6.85 s | 2.8 ms |
| `reuse` | 3.63 s | 3.24 s | 6.87 s | 3.0 ms |
| `pool` | 3.60 s | 0.27 s | 3.87 s | 3.3 ms |
| `arena` | 3.95 s | 3.38 s | 7.33 s | 3.8 ms |

`reuse` removed the mapping scans. Preparing newly locked addresses still
scanned `/proc/self/maps` during both passes. `pool` copied about 20.2 MiB
across 4,147 windows in 15.2 ms and never fell back from its slots. The warm
pass spent 1.6 ms on VMA checks, down from 3.01 s in `remap`. The pool kept
1 MiB resident. Cold cache publication still spent 3.06 s in `store_output`,
including the mapping checks that now occur there.

`arena` served 28 of 4,055 mapped windows directly. Most windows contained
several physical file runs and still required a regular mapping. This measures
the stable-address experiment on the current page layout. A design that
changes backing placement or lets windowed readers consume fragmented views
would need its own measurements. The pool is the strongest candidate for
reducing warm playback cost on this workload. Cold transaction tails and
performance across live edits remain open measurements. The 77 focused storage
tests passed, including the arena's pinned-reader page-replacement test.
