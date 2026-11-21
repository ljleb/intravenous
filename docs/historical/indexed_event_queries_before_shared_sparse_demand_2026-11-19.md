# Earlier indexed Event Region proposal

_Historical record, 2026-11-19. The owner clarified that sparse demand
must cover continuous streams as well as events. See the
[shared sparse Tock demand proposal](../sparse_tock_demand_proposal.md)._

From `docs/indexed_event_queries_proposal.md`.

## Keep time coverage and event indices distinct

Keep `IndexCoverage` as the set of positions on a port's native lattice.
It describes existence, change, and positional demand. A positional event
demand means **all occurrences at the requested positions**. Several events
can share a position.

An event index identifies one occurrence in one logical `EventRegion`.
Number events from zero in forward order from the Region's origin. Use the
producer's semantic order for events at the same timestamp. The index is
relative to a Region in a selected graph and data snapshot. A read of a
subrange, a cache eviction, and a split into physical storage chunks all
preserve that index. Inserting an event in a later snapshot can renumber every
subsequent event.

`EventCoverage` exposes the independent Region sequences in a stable order.
The positional union can discard overlap and multiplicity. The Region
references used by indexed reads preserve both.
[Global indexing](../global_indexing.md#explicit-orderings) ·
[Event ordering and fan-in](../coverage_and_background_evaluation.md#15-persisted-event-chunks-fan-in-order-and-random-access-iteration)

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

struct EventIndexRun {
    std::uint64_t first;
    std::uint64_t count;
    std::uint64_t step = 1;
};

class EventRegionRef {
    std::uint64_t snapshot_token_ = 0;
    std::uint64_t region_token_ = 0;
};

struct EventIndexSelection {
    EventRegionRef region;
    std::span<EventIndexRun const> runs;
};
```

1. **Forward propagation.** Overload `publish(EventRegionInfo)` on event
   Region outputs and `publish(std::span<EventRegionInfo const>)` on event
   Coverage outputs. Providing `index` promises the exact count and direct
   production of a selection of event indices. A port can publish both
   indexed and positional Regions. `max_event_count` remains a capacity
   bound. Publish indexed Regions when the producer can supply the count
   without an unbounded materialization during forward propagation. The
   planner can provide an indexed read of published data when the selected
   Region already has a searchable, countable sequence.
2. **Demand and discovery.** Event port facades expose the snapshot's Region
   references and their exact counts where available. Extend
   `ctx.input<"in">().require(selection)` and
   `co_await ctx.request_input<"in">(selection)` to accept
   `EventIndexSelection`. The output side exposes
   `ctx.output<"out">().required_event_indices()`, with the requested runs
   grouped by output Region. These extend the existing requirement and
   suspendable input discovery paths. `EventRegionRef` is issued by the
   planner, with tokens bound to the port, contribution, and selected
   candidate. A suspended request owns a copy of its run descriptors.
   An observation submits the same kind of root demand. References travel
   through the compiled routes of the receiving port, including public
   module inputs.
3. **Evaluation.** For an indexed demand, `tock_coverage*()` exposes
   `WritableEventRegion::requested_event_runs()`. Each run has
   `size()`, `event_index(slot)`, and `write(slot, TimedEvent)`.
   Prepared indexed input runs expose the same indices and a read-only span
   of their events. Storage has one densely packed entry per selected event.
   Existing positional event reads and writes continue to use
   `requested_coverage()` and the ordinary Region writer. A constant-payload
   Region exposes `write_time(slot, GlobalIndex)` on the indexed run and
   `write_time(GlobalIndex)` on the positional writer. These writes take
   the payload from the declared Region.

```cpp
for (auto region : ctx.output<"events">().regions()) {
    for (auto run : region.requested_event_runs()) {
        for (std::size_t slot = 0; slot < run.size(); ++slot) {
            run.write(slot, event_at_index(run.event_index(slot), ctx.sample_rate));
        }
    }
}
```

`write()` requires exactly one event for each selected index, with a timestamp
in the declared Region and the port's payload type. The result agrees with a
full positional read of the same snapshot, including the order of coincident
events. Sparse writes are checked for completeness and order before the
result becomes readable. The producer must support each form it advertises.
It can use the same event generator for ordinary positional and indexed
requests, with a different way to pick the indices to generate.

A run selects `first + slot * step` for `0 <= slot < count`. The planner
checks arithmetic, count, step, Region ownership, and version before scheduling
work. Zero entries are an empty selection. Nonempty runs require positive
steps and a last index below the Region's exact count. Within a Region, each
requested index has one destination slot, including when several requests
select it. The planner may normalize bounded selections to explicit indices
when runs overlap. Its work is bounded by the number of requested entries.

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

## Continuous samples

Continuous sample indices already exist as positions on the native lattice.
Their exact sparse demand belongs in `IndexCoverage`. Today
`ContinuousRegionView::prepared_windows()` gives one dense buffer for each
contiguous requested span. This is sufficient for correctness, including
the current bounded waveform queries.

If profiling justifies regular sparse packing, use a sample view of this
form inside Tock. Its stride is in native sample indices.

```cpp
template<class Value, bool Mutable>
class PreparedSampleRun {
public:
    std::size_t frame_count() const;
    GlobalIndex sample_index(std::size_t frame) const;
    // value(frame, channel) uses the same typed reference as ContinuousRegionView.
};
```

For example, positions `100, 104, 108` use three packed frames with a
position step of four. All channels and their planar or interleaved layout
keep their ordinary meaning. An indexed accessor gives each frame its
native position, and the Region keeps its full logical range. A pointwise
operation can request the same three input positions. A convolution can
request a larger neighborhood. A complete-buffer consumer still requires
every sample in its Region in native contiguous layout.

The application can implement event indices using this storage principle
before changing continuous storage. Enabling a strided sample view requires
an explicit callback contract. A callback that iterates
`prepared_windows()` must see all of its required frames as contiguous
windows. A future migration can move stride-capable consumers to
`prepared_runs()` and test them against identical positional demand.

## Caches, versions, and playback

- **Track completeness in the request's index space.** A sparse indexed
  result proves those event indices. A positional event window proves all
  occurrences at the covered positions, including emptiness. Two indexed
  results may share work for identical ranks in the same version. Schedule
  indexed and positional demands separately at first. Both can pin the
  same published source. Ordinary playback must have its exact positional
  window before Tick runs.
- **Keep invalidation positional for ordinary output data.** Cache indexed
  results against the pinned semantic and data versions and Region
  identity. An insertion can shift ranks even outside the position at
  which it occurred. Initially discard indexed results for a changed
  Region version. Reuse unchanged positional pages through the existing
  reconstruction rules. Rank metadata is a dependency of any forward
  or reverse callback that reads it.
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

1. Implement indexed Region publication, reverse requirements, packed Tock
   bindings, and seeking. Exercise it through an authored point list and
   a time transform before using the API from the beat observer. Verify
   finite curve lookups against the current interpolation, including cyclic
   boundaries, coincident events, and exact time ordering. A curve may still
   need many authored points in its forward pass to determine its exact
   active Coverage. Indexed reads only reduce dependencies that are
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
