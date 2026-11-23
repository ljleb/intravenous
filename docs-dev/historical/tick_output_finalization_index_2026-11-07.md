# Tick Sample Output Finalization Index

_Historical status: non-normative. This file preserves the design text that placed a
latent Tick sample output's values at their finalization index: storage filed a value
authored for position `P` at `P + latency`, and the output owned a latency/future
window at and after its next tick._

_Sources, preserved 2026-11-07:_
- _[Sequential Port Storage Planning](../sequential_port_storage_planning.md), section
  "Port windows are semantically node-owned even when storage is shared": the
  as-if-private-state list, the semantic-state inventory, and the physical
  realization paragraph._
- _[DSP Execution and Storage Glossary](../dsp_execution_storage_glossary.md), entry
  "Node-owned port state"._
- _[Graph JIT Direction](../graph_jit_direction.md), section "Port history and latency
  are node-owned semantic state", and the paragraph on executable replacement._

_Reason for replacement:_
- _The implementation never used this convention for sample outputs.
  `ContinuousOutputPort::value()` writes each value at its own sequential index, and
  `OutputPort::update()` revises the newest `latency` values before the current tick.
  Each consumer delays its reads by `read_latency`, which includes the source
  latency and which it already needs for path equalization. Lowering set
  `ContinuousPortStorageBinding::storage_latency` to 0 everywhere._
- _Cutover code was written for this text while steady execution followed the
  implementation: `make_transition()` placed the latency role at `[0, latency)`, the
  callback-facing output view carried a positive timeline offset, and the
  disconnected-output carry committed `future_frames = latency`. Front 16's
  `feedback` benchmark shape failed its first edit because of that mismatch
  ("sample transition source cannot represent its semantic interval")._
- _A destination comparison (Front 16 in
  [implementation_fronts_direction.md](../implementation_fronts_direction.md)) found
  that the finalization index adds an offset on every producer write and on
  persisted capture. It also splits each output's owned window around the next
  tick, which requires signed transition intervals or carried future frames. The
  authoring index keeps every sample output window before the next tick._

_Replacement (decided by the human co-author on 2026-11-07): a Tick sample output files
each value at the index it was authored for. Its owned state is one window, its last
`history + latency` values before its next tick. Latency marks the newest `latency` of
those values as still revisable through `OutputPort::update()`. Consumers apply the
delay through `read_latency`. Tick event outputs keep a separate latency role,
because an event output can stamp events at or after its next tick. See
[Sequential Port Storage Planning](../sequential_port_storage_planning.md), the section
"Port windows are semantically node-owned even when storage is shared"._

## Preserved list (sequential_port_storage_planning.md)

> The configured/resolved history/latency requirements should be interpreted by an
> **as-if private state** rule. For graph-revision semantics, each concrete node behaves
> as though its port windows were ordinary state owned beside its nested `State`:
>
> - a Sequential input owns the resolved values in its history window;
> - a Tick output owns the values in its resolved history window; and
> - a Tick output owns its already-authored values inside its resolved latency/future
>   window.

## Preserved inventory (sequential_port_storage_planning.md)

> Connection analysis therefore emits one cold semantic-state requirement for every
> non-empty owned extent before storage selection:
>
> ```text
> Sequential sample input history -> one record per semantic channel
> Sequential event input history  -> one record for the event stream
> Tick sample output history       -> one record per semantic channel
> Tick sample output latency       -> one record per semantic channel
> Tick event output history        -> one record for the event stream
> Tick event output latency        -> one record for the event stream
> ```

## Preserved physical realization paragraph (sequential_port_storage_planning.md)

> The physical realization view is a separate mapping produced after storage selection.
> For sample state, lowering now maps every semantic requirement to its finalized
> per-channel representation after `NodeLayout` finalization. The callback-facing view and
> cross-callback source are separate: a compact carry, for example, exposes a transient
> working ring to the node while retaining only its bounded tail in `NodeStorage`. The cold
> record distinguishes an immutable constant, callback-local arena storage, compact
> cross-callback carry, and a persistent ring. Each physical view retains its channel
> layout/channel, signed semantic-to-storage timeline offset, working capacity, actual
> stored-frame count, compact-carry future extent, and final `NodeStorage` or
> callback-arena-relative byte range. For semantic coordinate `P`, the physical ring
> coordinate is `P + offset`: Sequential-input read latency/channel delay is negative,
> while Tick-output authored storage latency is positive.

## Preserved glossary sentence (dsp_execution_storage_glossary.md)

> **Node-owned port state** is the semantic history/latency state of a surviving
> concrete node's ports across graph revisions. A Sequential input owns its resolved
> history; a Tick output owns its resolved history and latency/future window.

## Preserved ownership tree and paragraph (graph_jit_direction.md)

> ```text
> concrete node
>     +-- authored State
>     +-- each Sequential input's resolved history
>     +-- each Tick output's resolved history
>     `-- each Tick output's resolved latency/future window
> ```

> Likewise, a surviving output owns its resolved history and latency/future state.
> Changing consumers must not discard that state.

## Preserved executable-replacement sentence (graph_jit_direction.md)

> Executable replacement must preserve concrete-node-owned port state. Input history
> belongs to the surviving destination input; output history and authored latency/future
> state belong to the surviving source output.
