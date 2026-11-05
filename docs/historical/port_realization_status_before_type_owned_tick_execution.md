# Port-Realization Status Before Type-Owned Tick Execution

_Status: non-normative implementation checkpoint preserved on 2026-10-17 from
`docs/port_realization_migration_status.md`._

This checkpoint preserves the incomplete-milestone and next-connection text that was
superseded when the working tree acquired type-owned continuous storage, per-port
dyadic projections, and concrete Tick/Sequential port access. The current status is
maintained in
[Port Realization Migration Status](../port_realization_migration_status.md).

The same status previously described the sample-named execution/storage planner as an
"explicitly unfinished realization layer." That wording referred to missing generic
behavior, not merely its filenames. The behavior was subsequently generalized; the
remaining sample-named identifiers are terminology debt rather than a second runtime
implementation.

It also stated that existing Sample planning consumed type-owned metadata that
"future continuous-value realizations" would use. Those realizations are the work
whose completion superseded this checkpoint.

## Previously reported incomplete milestones

- GraphJit scheduling still invokes every node with one common project block extent.
  It does not yet project the root's selected dyadic interval onto each port lattice or
  present the resulting zero-or-more values to each node.
- Tick bindings and authored port facades still address Sample storage with the old
  one-frame-per-project-index assumption. They do not yet expose per-port transported
  `block_size()` or the resolved `block_extended()` Sequential view.
- Source introspection cannot report the selected value type, resolved dynamic size,
  or live runtime value. Those facts become available only after GraphJIT realization
  and require the planned ProjectGraph/runtime read-model join.
- Port pace is solved, and per-value sample support is resolved from the selected type.
  Tick execution still has to apply those facts to per-port intervals and storage.
  The project sample rate remains one unchanged context parameter; per-port sample-rate
  basis solving and fractional resampling are not part of this front.
- Coverage ordering is solved and propagated among Coverage endpoints. Region ports
  carry no ordering variable because a finite Region is intrinsically ordered. The
  Region/Coverage realization that satisfies a sorted Coverage requirement remains
  owned by Front 3.

## Previously planned next bottom-up connection

Complete type-owned storage and node-facing port access for the resolved continuous
type, starting with `FractionalGlobalIndex` and the canonical `FFTBlock`
representation. The existing Sample execution planner must not acquire central
switches for those types: each extension supplies its storage/access operations, while
realization selects and specializes one alternative before lowering. After that
storage/access path works, carry projected per-port extents into generated Tick
bindings and storage planning.
