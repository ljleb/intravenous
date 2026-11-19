# Sparse Tock demand for continuous streams and events

_Proposal, 2026-11-19. The owner has accepted
[Timeline nodes and project snapping](./project_playback_and_presentation_grids_direction.md#timeline-nodes-and-project-snapping).
The API and implementation order below are proposed._

## Addressing selected entries by Region

Use one selection API for continuous and event ports. A selection names a
logical Region and a set of Region-relative index runs. The port determines
the meaning of an index.

- A **continuous index** is a frame offset on the port's native lattice,
  relative to the Region's begin. Selecting it requests that frame for the
  channels resolved by the connection or observation.
- An **event index** identifies one occurrence in the Region's ordered event
  sequence. Number events from zero in forward order from the Region origin.
  Use the producer's semantic order for events at the same timestamp.

Keep `IndexCoverage` as the set of positions for existence, change, and
positional demand. A positional event demand selects all occurrences at the
requested positions. A continuous positional demand selects the frames in the
specified positions for each affected Region. Region-relative selections
retain both the individual Region and the exact entries requested within it.
Keep stride runs compact throughout planning and storage.

Indices refer to a logical Region in a selected graph and data snapshot.
Subrange reads, cache evictions, and physical storage splits preserve them.
Inserting an event in a later snapshot can renumber subsequent events.
[Global indexing](./global_indexing.md#explicit-orderings) ·
[Event ordering and fan-in](./coverage_and_background_evaluation.md#15-persisted-event-chunks-fan-in-order-and-random-access-iteration)

## Authored API

These declarations describe the proposed public surface. The reflected
records bind the same operations to a candidate and a prepared Tock call.

```cpp
struct IndexedEventRegionInfo {
    std::uint64_t event_count;
    std::optional<Event> constant_value;
};

struct EventRegionInfo {
    GlobalIndexRange range;
    std::optional<IndexedEventRegionInfo> index;
};

struct IndexRun {
    std::uint64_t first;
    std::uint64_t count;
    std::uint64_t step = 1;
};

class RegionRef {
    std::uint64_t snapshot_token_ = 0;
    std::uint64_t region_token_ = 0;
};

struct RegionSelection {
    RegionRef region;
    std::span<IndexRun const> runs;
};
```

1. **Forward propagation.** Continuous outputs already publish the logical
   Region range, so their index count is its extent on the native lattice.
   Event outputs can publish `EventRegionInfo` for a Region or a span of those
   records for Coverage. Providing `index` promises an exact event count and
   direct production of selected indices. A port can publish both indexed
   and ordinary positional event Regions. `max_event_count` remains a
   capacity bound. Publish indexed Regions when the count is available
   without unbounded materialization. The planner can provide indexed reads of
   published recordings with a searchable, countable sequence.
2. **Demand and discovery.** Both port kinds expose their Region references
   through `request_input_coverage()`. The planner binds each reference to
   its port, contribution, and pinned candidate. Both accept
   `ctx.input<"in">().require(selection)` and
   `co_await ctx.request_input<"in">(selection)`. A suspended request owns
   a copy of its runs. Both output facades expose
   `ctx.output<"out">().required_selections()` alongside positional
   `required()`. Reverse propagation must account for both. An observation
   can submit either form as a root demand. Module inputs carry selections
   through the compiled receiving routes.
3. **Evaluation.** The logical Region's `prepared_runs()` exposes the
   selected entries as dense storage. Both run types have `size()` and
   `index(slot)`, which returns the Region-relative index. A continuous
   run adds `position(slot)` and typed `value(slot, channel)` access.
   An indexed event run provides a read-only span of `TimedEvent`s or
   a writable `write(slot, TimedEvent)` on an output. A constant-payload
   Region also exposes `write_time(slot, GlobalIndex)`, using its declared
   payload. A positional event request still calls the Region writer in
   timestamp order because its occurrence count can be discovered during
   production.

```cpp
// A continuous output evaluates only the selected positions.
for (auto region : ctx.output<"value">().regions()) {
    for (auto run : region.prepared_runs()) {
        for (std::size_t slot = 0; slot < run.size(); ++slot) {
            run.value(slot, 0) = value_at(run.position(slot), ctx.sample_rate);
        }
    }
}

// An indexed event output evaluates only the selected occurrences.
for (auto region : ctx.output<"events">().regions()) {
    for (auto run : region.prepared_runs()) {
        for (std::size_t slot = 0; slot < run.size(); ++slot) {
            run.write(slot, event_at_index(run.index(slot), ctx.sample_rate));
        }
    }
}
```

Both forms must fill every requested slot with the same value a full read
would return. An event write also preserves the payload type, the time within
its Region, and the semantic order of coincident events. The runtime validates
writes before a result becomes readable.

A run selects `first + slot * step` for `0 <= slot < count`. For a continuous
Region starting at native index 100, `{0, 3, 4}` packs positions 100, 104,
and 108 into three frames. For an event Region, the same run selects
occurrences 0, 4, and 8, each with its original timestamp.

Check arithmetic, nonempty positive steps, last index, Region ownership, and
snapshot version before scheduling. An empty run requests zero entries.
Clip positional requests to each Region. An explicit Region selection must
already fit that Region. Within a Region and channel selection, each
requested index has one output destination even when requests overlap.
Keep nonoverlapping regular runs compact. Normalize overlaps when the
finite work budget permits it. Separate Region references retain separate
entries even when the Regions cover the same positions.

## Seek and propagation

An indexed Region has enough information for the scheduler to find the
index of the first event at or after a time. Search its finite index interval
with bounded timestamp probes through the same indexed read path. Compare
timestamps relative to the Region origin. For a time-window query, first
intersect with that Region and split the intersection into intervals ordered
relative to its origin. An interval ending at the Region's exclusive end uses
the exact event count as its upper index. A selected backing store with a
direct search can answer immediately. For computed output, the scheduler can
batch each search round across independent Regions and queries. A binary
search can still need up to 64 rounds, so measure dispatch and reverse
propagation costs. This proposal uses the existing Tock callback and
scheduler for those probes.

Add a suspendable `request_event_neighbors<"in">(region, position, mode)`
which uses the same search. `EventRegion::neighbors()` remains a synchronous
lookup on already prepared data. For neighbors use the first index strictly
after the position and its predecessor. Cyclic lookup can use the first or
last indexed event at a Region edge. A Region with zero events returns an
empty result. Unavailable data reports a separate failure or pending state.

| Operation | Forward guarantee | Reverse requirement |
| --- | --- | --- |
| Authored point list | Exact Region and count from the selected data | Direct stored access by index |
| Constant time shift | Shifted positions with the same order and count | The same event indices on the corresponding input Region |
| Ordered timing deformation | Exact output Region and count when order is preserved | The same indices if the deformation evaluates independently at those indices |
| Event filter that changes count | Indexed only if it can publish the exact count and resolve selected output indices | The corresponding input indices from its own index, or positional demand for the input data it must inspect |
| Live or non-replayable input | Published recording and its available Regions | Reads from that recording, within its availability |

For example, a filter that keeps every third event can map output index
`i` to input index `3*i` for a fixed phase. A predicate on arbitrary payloads
may need to examine the entire prefix before it can answer the same question.
It can use an exact computed index, or supply ordinary positional reads.
A time deformation can discover an input event at index `i`, then require
continuous control data at that event's original position. To publish an
indexed output it also needs a sound ordering guarantee for the deformed
positions.
`EventTypeId` identifies the payload. The resolved Region describes which
read guarantees actually hold through that graph route.

Existing event conversions can discard occurrences. A route preserves an
index only when conversion, time mapping, clipping, and fan-in preserve
the sequence and its order. Region-preserving fan-in carries each index
separately. A clipped or filtered Region that exposes its own index must
map that index explicitly. The compiler can reject an indexed request
for an unsupported route while ordinary connections remain legal.

## Continuous propagation and storage

`IndexCoverage` continues to describe existence and changes. Put sparse
sample selections in `RegionSelection`. The planner can intersect a run
with a positional range using arithmetic on its endpoints. This avoids
expanding a regular run into a set of one-frame windows and then packing it
again. Dense positional demands produce step-one runs and keep their
existing contiguous storage fast path.

A pointwise transform can map a selection directly through reverse
propagation. A constant delay translates its positions and preserves its
step when both sides use the same lattice. A pace change must project
onto the input lattice and request each sample the conversion requires.
A convolution requests the neighborhoods it needs around the selected
outputs. Such input dependencies can be wider or denser than the produced
output. Packed storage offsets never determine a filter's timeline
distances.

Each continuous run retains the current native channel layout. Planar
channels pack only the selected frames of that channel. Interleaved
storage packs the selected complete frames for the requested channels.
For arbitrary-position reads, `at(position, channel)` checks exact
membership and locates the corresponding packed slot. A complete-buffer
consumer still requires all native frames in one contiguous buffer.
A run with a step above one exposes its position mapping without acting as
a contiguous time range.

Migrate continuous Tock kernels to `prepared_runs()` and
`required_selections()` as part of the shared API change. This can replace
`prepared_windows()` in kernels that iterate the prepared output. Keep
whole-Region and explicitly contiguous accessors for consumers whose
algorithms require them. An algorithm that needs contiguous input can
request it through reverse propagation. If it also needs contiguous output
evaluation, the planner can prepare a valid positional window and gather
the selected frames from it. Apply the observation work budget to the
whole window in that case.

Use the waveform observer as the first continuous client. It already picks
representative sample positions. Preserve the positions it chooses, express
regular portions as runs, and use shorter runs for irregular spacing.
The representative waveform remains approximate as a display of the whole
signal. Each selected sample value is exact.

## Caches, versions, and playback

- **Track completeness in the request's index space.** A sparse
  continuous result proves those native positions and channels. An indexed
  event result proves those event indices. A positional event window proves
  all occurrences at the covered positions, including emptiness. For
  continuous streams the planner can coalesce positional and indexed demands
  for the same Region and channels. For events, schedule indexed and
  positional demands separately until their completeness can be established
  in both spaces. Both can pin the same published source. Playback requires
  its exact positional window before Tick runs.
- **Keep invalidation positional for continuous output data.** Reuse exact
  continuous positions outside changed ranges through existing
  reconstruction rules. Cache event-index results against the pinned
  semantic and data versions and Region identity. An insertion can shift
  ranks even outside the position at which it occurred. Initially discard
  event-index results for a changed Region version. Event count and index
  metadata are dependencies of any forward or reverse callback that reads
  them.
- **Use the existing background owner.** Resolve the selection and all
  upstream dependencies against one snapshot. Evaluate bounded work
  between higher-priority playback preparations. Bound selected entries,
  search probes, upstream materialization, and pinned bytes. A small reply
  can still have expensive dependencies. An arbitrary Tock callback can
  spend time inside one invocation, so these limits bound admitted work
  and allocations rather than guaranteeing a deadline on that callback.
  Reuse a still-valid result while serving a newer viewport request.
- **Keep publication and realtime semantics.** A selected result can use
  transaction-local storage or a pinned published source. Publish an
  observation only after its dependencies have resolved for its version.
  Optional observation caches reclaim results under pressure. Recorded
  data retains its authoritative storage. A Sequential consumer continues
  to receive every accepted event in its prepared window, including
  coincident events in semantic order.

The authored facades, reflected Tock bindings, and event materialization
planner change together. Region references live in the selected runtime
snapshot. Project persistence still records the graph and its authored
data. Neither project files nor frontend layout files carry runtime event
indices or Region references.

## Beat hierarchy and Timeline

An index alone cannot find all events with `BeatEvent.depth <= D`. It
tells us where to read an occurrence. It gives no information about which
indices satisfy an arbitrary predicate on the payload.

I propose that a structured beat output use **one indexed Event Region per
depth in each logical span**. To make a level discoverable without scanning
it, an indexed Region can declare a common payload at publication.
For example, add an overload of `publish_indexed` that accepts a Region
description with `Event constant_value`. For that Region the writer takes
only timestamps and the runtime supplies the constant value. This has the
same meaning for a Region of `TriggerEvent`s, repeated `SampleEvent`s, or
`BeatEvent`s at a fixed depth. It is an exact event representation rather
than an arbitrary annotation or filter callback.

This changes the logical Region list of `Beats`. Sequential readers still
receive the same occurrences. The builtin emits at most one event per
sample. If a transform creates coincidences, its output Region order and
the event order within each Region determine the sequential tie break.
A Coverage reader can query each level independently. A transform which
changes the common payload declares its own value or uses ordinary event
payloads. A secondary depth index could preserve the current beat Region
list at the cost of a new index-maintenance contract and query dimension.
This proposal favors publishing the levels as Regions.

`Timeline` is a module with a public BeatEvent Coverage input and no DSP
callback. The existing module port path supplies its connections. Input
observation needs a generic public Event Coverage read view, analogous to
the existing sample view, because a passive input has no scheduled
consumer. Project-wide discovery returns Timeline addresses, including
addresses inside subgraphs and outside the visible scope.

Register `beat-grid` for `EventTypeId::beat` in event observation. Its
request names a node, an Event input member, and
`{ begin, length, maxDepth, maxMarks }`. Carry the port kind through
resolution, since a Sample input and Event input can share a member name.
Encode timeline coordinates as integer project sample strings on the wire.
The operation submits ordinary indexed demands. It has no producer callback
of its own. The client uses the existing versioned observation subscription
path, with bounded batches for all Timeline inputs.

For each Region, seek to the first and past-last indices in the viewport.
Include levels up to the inclusive `maxDepth`, using the common payload.
Choose the deepest prefix of those levels that fits `maxMarks`. If even
the coarsest level exceeds the budget, thin it with a power-of-two index
stride aligned to the logical Region origin. If the aligned marks still
exceed the budget at the largest representable stride, report that limit
for the query. Phase stays stable during panning within a Region. Coarse
marks from independent logical Regions can have different phases, including
adjacent Regions from the same producer. Physical storage splits preserve
phase. A mixed or unindexed Region uses a bounded exact read when available.
Report ranges that cannot be read within the budget.

Pin one semantic and data snapshot for all routes in an observation batch.
Keep aggregate backend and reply budgets for the batch even when it
contains many Timeline inputs. Supersede queued requests when the source
version, scope, or viewport changes. Deliver completed ranges with their
version, so the UI can reject stale results and distinguish them from a
covered range with no marks.

Report exact sample positions on the receiving grid alongside depth,
source version, and the range actually answered. The UI can combine
coincident marks from all contributions and apply its own display
spacing, including when irregular timing clusters a regular index stride
in a few pixels. The ruler and point editing consume the same list.

## Implementation and acceptance

1. Implement the common Region selection, reverse requirements, and packed
   Tock bindings for both port kinds. For continuous samples, verify the
   waveform and a multi-channel transform against an ordinary positional read.
   Check stride one, overlapping Regions, pace mapping, and a convolution
   whose inputs require a wider range. For events, add exact index publication
   and time seek. Test an authored point list and a time transform. Check
   cyclic boundaries, coincident events, and the current interpolation.
   A curve's forward pass can still need many authored points to determine its
   exact active Coverage. Indexed reads reduce dependencies that are
   actually sparse.
2. Implement passive public Event Coverage input observation. Verify multiple
   overlapping Regions, duplicate routes, transforms, unavailable inputs,
   changes of snapshot, and an observer that comes and goes during playback.
3. Implement the beat Region representation and Timeline UI. On a cold
   zoomed-out read, measure the number of event indices evaluated, upstream
   bytes, and scheduler rounds so a short reply cannot hide a wide
   materialization. Test regular and order-preserving noisy grids, two
   contributions into one input, and Sequential playback of the same
   graph. Verify snapping agrees with the drawn marks.

Boundary pairing and MIDI state at a window edge require information about
the preceding sequence. Event indices help find and select occurrences.
A separate state reconstruction operation would need a defined initial
state and exact preceding events or a versioned checkpoint. Those semantics
can be designed independently from indexed reads.
