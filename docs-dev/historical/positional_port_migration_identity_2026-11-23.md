# Positional port migration identity before named-port matching

_Historical, non-normative. Preserved on 2026-11-23 when the owner approved
matching unique named ports across index changes._

From [GraphJit direction](../graph_jit_direction.md), which now specifies the
name rule for compiler-managed port state.

> Compiler-managed port-state identity must therefore be rooted in the stable concrete
> node path already present in configured/project graph metadata: user-instantiated
> leaf/module identity, symbolic-node/direct-member path, port direction and index,
> channel index (or event stream), plus a small state-role discriminator such as
> `input_history`, `output_history`, or `output_latency`. Ordinary connection identity,
> allocation number, incidence-atom index, buffer kind, capacity, and byte offset are
> not part of that identity. A connection may disappear or change while the destination
> input state survives.

From [Sequential port storage planning](../sequential_port_storage_planning.md),
which now describes the same name rule for state migration.

> The stable migration identity belongs to the concrete node port, not the
> connection or storage plan: user-instantiated node/module identity, symbolic-node and
> concrete-member path, port direction/index, channel index or event stream, and a
> state-role discriminator. Port incidence classes, connection IDs, allocation
> indices, compact/ring kind, capacity and offsets are generation-local storage facts.
> Any storage plan that aliases/elides one of these conceptual state pieces must retain
> cold metadata capable of reading that semantic window again during a later graph
> transition.
