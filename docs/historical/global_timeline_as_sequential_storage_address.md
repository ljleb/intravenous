# Global Timeline As Sequential Storage Address

_Status: non-normative historical record preserved on 2026-10-17 from
`docs/global_indexing.md`, `docs/graph_jit_direction.md`,
`docs/sequential_port_storage_planning.md`, and
`docs/port_realization_migration_status.md`._

_Reason for supersession: these passages treated the global timeline coordinate and
the physical ordinal of Sequential retained storage as the same number. That model
cannot preserve Sequential history across a pause/resume or seek seam, because the
next node-visible global coordinate may jump while invocation-order history must remain
adjacent. The accepted replacement is documented in
[Circular Global Timeline Indexing](../global_indexing.md#sequential-execution-order)
and the current Sequential storage design._

The superseded interpretation was:

- `FractionalGlobalIndex` addressed both multirate execution and continuous storage;
- a port's projected global-lattice ordinal directly selected its physical ring slot;
- `block_extended()` meant the globally adjacent `history()` values immediately before
  the callback's current global interval;
- a Tick callback at global origin `B` described history/current/latency as the single
  modular range beginning at `B - H` with extent `H + N + L`; and
- initialized feedback/alignment rings needed no authored-frontier or activation
  distinction because newly produced global positions were expected to overwrite
  initialized prehistory in monotonically increasing global order.

That model remains valid inside one uninterrupted global segment. It is not the general
Sequential contract. In the replacement design, `FractionalGlobalIndex` remains the
only node-visible time coordinate, while compiler-owned linear cursors address physical
Sequential storage in callback order.
