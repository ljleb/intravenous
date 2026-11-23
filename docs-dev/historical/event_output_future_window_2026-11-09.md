# Tick Event Output Future Window

_Historical status: non-normative. This file preserves the design text under which a
latent Tick event output stamped events at or after its next tick. Its latency was a
future-publication window: a callback beginning at `B` could author events in
`[B - history, B + block + latency)`, and the output owned the events it had already
stamped in that future window as state separate from its history. Event consumers read
each event at its own timestamp._

_Sources, preserved 2026-11-09:_
- _[Sequential Port Storage Planning](../sequential_port_storage_planning.md):_
  - _section "Port windows are semantically node-owned even when storage is shared":
    the two Tick event output entries of the as-if-private-state list, and the event
    lines of the semantic-state inventory;_
  - _section "Tick event ports need bounded time windows": the window statement and
    the legal output range;_
  - _section on event storage decisions: the paragraph on a cyclic producer with
    source history or latency._
- _[Graph JIT Direction](../graph_jit_direction.md), section "Port history and latency
  are node-owned semantic state": the conceptual ownership tree and the paragraph
  after it._
- _[DSP Execution and Storage Glossary](../dsp_execution_storage_glossary.md), entry
  "Node-owned port state"._

_Reason for replacement:_
- _Sample streams had moved to the authoring-index convention on 2026-11-07
  ([tick_output_finalization_index_2026-11-07.md](tick_output_finalization_index_2026-11-07.md)):
  a Tick sample output files each value at the index it was authored for, latency
  marks its newest values as still revisable, and each consumer reads through its
  `read_latency`. Event streams kept the opposite convention, so the two stream kinds
  differed in which positions an output owns, when its data becomes final, whether a
  consumer is delayed by the source latency, whether latency compensation applies,
  and whether an input owns in-flight data._
- _A comparison during Front 16 (see
  [implementation_fronts_direction.md](../implementation_fronts_direction.md)) found
  that these differences were not required by any intended observable behavior and
  had produced separate, diverging implementations of graph-revision continuity for
  the two stream kinds: event inputs owned no in-flight events, event inputs were not
  latency-compensated, and event detach structures were identified by a different
  rule._

_Replacement (decided by the human co-author on 2026-11-09): sample and event streams
share one lifetime and ownership model, and the normative documents name the only
places where they differ: what one position holds, what a fresh position holds, and how
several contributions to one position combine. A Tick event output authors in
`[B - latency, B + block)`, owns its last `history + latency` positions before its next
tick, and each consumer reads its events through its `read_latency`. See
[Sequential Port Storage Planning](../sequential_port_storage_planning.md), sections
"Sample and event streams share one lifetime and ownership model" and "Tick event ports
need bounded time windows"._

## Sequential Port Storage Planning: as-if-private-state list (event entries)

```text
- a Tick event output owns the events in its resolved history window; and
- a Tick event output owns the events it has already stamped inside its resolved
  latency/future window, at or after its next tick.
```

## Sequential Port Storage Planning: semantic-state inventory (event lines)

```text
Sequential event input history          -> one record for the event stream
Tick event output history               -> one record for the event stream
Tick event output latency               -> one record for the event stream
```

## Sequential Port Storage Planning: "Tick event ports need bounded time windows"

Tick event outputs must have a statically predictable temporal window just
like Tick sample outputs.

A Tick output callback must not be able to produce an event at an arbitrary
absolute time unrelated to the current invocation. Its legal output timestamps
must lie inside the finite window defined by the current block together with the
port's declared history and declared/corrected latency. In other words, Tick
event production is constrained by the same `current block + history + latency`
semantic extent used to make sequential sample access predictable.

For a callback beginning at `GlobalIndex B`, block size `N`, output history `H`,
and effective/corrected output latency `L`, the legal Tick event output range is:

```text
begin  = B retreated by H
extent = H + N + L
```

The range is half-open in forward modular order and may cross bit-pattern zero.
It is never clamped or saturated. The compatibility runtime enforces this exact
convention. Lowering may remove redundant checks when authored/generated accesses
are statically proved to stay inside the same range.

## Sequential Port Storage Planning: cyclic producer paragraph

For a cyclic producer with source history or latency, the callback writes an
invocation-local bounded sequence. A slice-scoped stable merge inserts it into
the canonical retained aggregate. This is necessary because a later slice may
legally author an event whose timestamp precedes a future event authored by an
earlier slice; direct append would violate the sorted-stream invariant. The same
mechanism extends to same-SCC fan-in. It uses only statically sized buffers. The
local capacity is derived from `SCC quantum + history + latency`; the aggregate
capacity accounts for the root block plus the history/latency allowance for
every possible slice, since each callback invocation may legally fill its whole
declared authored window.

## Graph JIT Direction: "Port history and latency are node-owned semantic state"

```text
concrete node
    +-- authored State
    +-- each Sequential input's resolved history and in-flight values
    +-- each Tick sample output's last history + latency authored values
    +-- each Tick event output's resolved history
    `-- each Tick event output's resolved latency/future window
```

A Tick sample output files each value at the index it was authored for, so all of its
owned values lie before its next tick; latency only marks the newest of them as still
revisable through `OutputPort::update()`. A Tick event output can stamp events at or
after its next tick, so its latency/future window is separate state. (Settled
2026-11-07; the superseded wording is preserved in
[historical/tick_output_finalization_index_2026-11-07.md](tick_output_finalization_index_2026-11-07.md).)

## DSP Execution and Storage Glossary: "Node-owned port state" (event sentence)

A Tick event output owns its resolved history and the events it has already stamped
in its latency/future window.
