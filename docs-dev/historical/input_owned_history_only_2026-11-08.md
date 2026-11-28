# Sequential Input Owns Only Its History

_Historical status: non-normative. This file preserves the design text under which a
Sequential input owned only the values in its history window. The values its source
had already finalized, but that compiler-owned read compensation had not yet let the
input read, belonged to no node-owned state._

_Sources, preserved 2026-11-08:_
- _[Sequential Port Storage Planning](../sequential_port_storage_planning.md), section
  "Port windows are semantically node-owned even when storage is shared": the first
  bullet of the as-if-private list, and the input line of the semantic-state
  inventory._
- _[Graph JIT Direction](../graph_jit_direction.md), section "Port history and latency
  are node-owned semantic state": the input line of the ownership tree, and the
  sentence of the rewiring example that sends positions from `P` onward to the new
  connection._
- _[DSP Execution and Storage Glossary](../dsp_execution_storage_glossary.md), entry
  "Node-owned port state"._

_Reason for replacement:_
- _A consumer reads its source at `index - read_latency`, where `read_latency` is the
  source latency plus compiler-owned read compensation. The source values in
  `[P - read_latency, P - source latency)` at a cutover position `P` are final but not
  yet read. No semantic-state record covered them, so the cutover transfer
  zero-filled them, or let another record's transfer overwrite them, whenever the
  producer storage's raw-region identity changed. Three tests reproduce this:
  `CompensatedInputUnaffectedBySecondReaderMove`,
  `CompensatedInputUnaffectedByHistoryReaderMove`, and
  `CompensatedHistoryInputUnaffectedByHistoryReaderMove` (graph_jit_gtest.cpp)._
- _The human co-author required that these in-flight values survive an edit that
  does not change the input's read delay. Graph transitions use no structural graph
  matching, so the values cannot belong to a connection; they must belong to a node
  port._

_Replacement (decided by the human co-author on 2026-11-08): the Sequential input owns
its in-flight values beside its history, as one window of `history + compensation`
values. A rewired input plays its in-flight values before the new source's values. See
[Sequential Port Storage Planning](../sequential_port_storage_planning.md), the section
"Port windows are semantically node-owned even when storage is shared"._

## Preserved list bullet (sequential_port_storage_planning.md)

> - a Sequential input owns the resolved values in its history window;

## Preserved inventory line (sequential_port_storage_planning.md)

> ```text
> Sequential sample input history         -> one record per semantic channel
> ```

## Preserved ownership tree line (graph_jit_direction.md)

> ```text
>     +-- each Sequential input's resolved history
> ```

## Preserved rewiring sentence (graph_jit_direction.md)

> then immediately after activation `C.in[P-64, P)` remains the resolved history that C
> actually observed before the splice, while positions from `P` onward use the new
> connection.

## Preserved glossary sentence (dsp_execution_storage_glossary.md)

> A Sequential input owns its resolved history.
