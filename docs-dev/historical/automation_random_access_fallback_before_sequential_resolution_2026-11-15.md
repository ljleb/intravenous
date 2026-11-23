# Random-access fallback proposals before Sequential resolution

_Historical, non-normative. Preserved on 2026-11-15 after the owner replaced
the earlier Region/Coverage fallback and resolved-read proposals._

Source: the in-progress automation direction, section 4, and migration plan.
The final decision preserves absence in Region/Coverage inputs and puts
neutral/manual fallback exclusively in Sequential resolution. Replacement:
[automation direction, section 4](../automation_and_suspendable_coverage_direction.md#4-absence-and-sequential-fallback).

## Superseded proposed contract

## 4. Uncovered-value policy belongs to the destination input

_Settled access-mode rule from the repository owner, 2026-11-15. This
supersedes the earlier Sequential manual-fallback proposal preserved in
[historical documentation](./historical/automation_manual_fallback_before_random_access_policy_2026-11-15.md)._

`empty` outgoing segments are **uncovered**, not covered with a zero sample
and not an interpolation toward a fallback value. Source coverage stays exact.

For sparse automation, **Region and Coverage destination inputs default to
manual-value fallback** on known-uncovered positions. The destination input
can override that behavior. This is a destination declaration policy, rather
than a setting on a project connection or a property inferred from a builtin
node ID. The input's manual value remains independent of the connected source.

**Sequential destination inputs always use their neutral value on uncovered
positions.** They cannot opt into this sparse-automation manual-fallback policy.
This rule concerns uncovered-position selection; temporary live knob overrides
and authored fan-in participation are separate mechanisms.

Fallback occurs only on **known uncovered** positions, never merely because
coverage discovery is pending. Pending discovery retains the previously
accepted playback environment until coherent replacement data are ready.

### Raw Region access and resolved value reads

_Settled by the owner on 2026-11-15. The earlier accessor proposals are
preserved [historically](./historical/automation_fallback_accessors_before_resolved_reads_2026-11-15.md)._

The two operations are complementary:

| Accessor | Meaning |
| --- | --- |
| `input.region()` / `input.regions()` | Actual supplied upstream Regions, preserving identity, ordering, overlap and multiplicity |
| `input.coverage()` | Positional union of exactly those supplied Regions; fallback is excluded |
| `input.read(index)` | Effective input value, applying the destination's fallback policy where appropriate |

For a Sample input, a resolved read returns the upstream contribution when
covered, and the selected manual/default value when the index is **known to
be uncovered**. A read outside the permitted evaluation domain is rejected.
Missing selected materialization is not evidence of missing source coverage.
The scheduler retains the information needed to distinguish these states
internally; no authored raw-request accessor is needed.

For overlapping Coverage contributions, a scalar resolved read must use an
explicitly declared combination rule. Otherwise the read interface must expose
the contributions rather than arbitrarily choosing one. Fallback applies only
when no source contributes; it is not an additional fan-in contribution. The
resolved-read interface can vary with the continuous value type.

Fallback never creates upstream Regions or fills Region iteration with synthetic
structure. A node that iterates only `regions()` processes only actual supplied
data. A parameter-processing node can instead use resolved reads over its output
domain. Nodes do not have to reimplement coverage tests and fallback selection.
At the same semantic snapshot and permitted request, cache history cannot change
the resolved value.


## Previous implementation checkpoint

### Resolved reads accepted, 2026-11-15

The owner selected complementary raw Region access and resolved value access.
`region()`/`regions()` retain supplied Regions and `coverage()` is their exact
positional union. `input.read(index)` applies the destination policy only on
known-uncovered positions in its permitted evaluation domain. It rejects
unselected or unmaterialized data rather than treating it as a gap. Overlap
requires the input's explicit combination rule or a contribution-preserving
read; fallback is not another contribution. No fallback Regions are fabricated.

Equal-index points form an ordered jump group. The point source will break ties
using persistent IDs; the interpreter uses stable event order and keeps IDs out
of the event payload. At the shared index, the last point's outgoing semantics
apply. The earlier open accessor question is retained in
[historical documentation](historical/automation_fallback_accessors_before_resolved_reads_2026-11-15.md).

Implementation resumes with resolved read domains/policies, followed by the
CurvePointEvent source/interpreter and presentation migration. This is accepted
intent, not a statement that these new interfaces already exist in source.
